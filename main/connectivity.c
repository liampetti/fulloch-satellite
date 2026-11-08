#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "connectivity.h"
#include "audio.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "protocol.h"
#include "ui.h"

#define WIFI_RECONNECT_INITIAL_MS 1000U
#define WEBSOCKET_RECONNECT_INITIAL_MS 1000U
#define RECONNECT_MAX_MS 60000U
#define WEBSOCKET_STABLE_CONNECTION_MS 30000U
#define WEBSOCKET_SHORT_LIVED_CONNECTION_LIMIT 3U
#define STARTUP_SERVER_ATTEMPTS 3U
#define SERVER_RECHECK_MS 60000U
#define WEBSOCKET_OPCODE_CLOSE 0x8
#define WEBSOCKET_TASK_STACK_BYTES 8192
// ESP-IDF waits for its WebSocket worker to leave a network read during stop.
// Keep that read bounded so a failed transport cannot delay our reconnect loop.
#define WEBSOCKET_NETWORK_TIMEOUT_MS 5000
#define CONNECTIVITY_CHECK_MS 60000U
#define CONNECTIVITY_STALL_RESET_MS 600000U
#define SERVER_INBOUND_STALL_MS 90000U
#define WIFI_CONNECT_TIMEOUT_MS 30000U
#define WEBSOCKET_WELCOME_TIMEOUT_MS 10000U
#define CONNECTIVITY_TASK_STACK_BYTES 6144
#define CONNECTIVITY_COMMAND_QUEUE_LENGTH 16

static const char *TAG = "connectivity";

static esp_websocket_client_handle_t websocket;
static QueueHandle_t connectivity_commands;
static esp_timer_handle_t wifi_reconnect_timer;
static esp_timer_handle_t websocket_reconnect_timer;
static esp_timer_handle_t connectivity_check_timer;
static esp_timer_handle_t wifi_connect_timer;
static esp_timer_handle_t websocket_welcome_timer;
static satellite_connection_status_t connection_status = SATELLITE_CONNECTION_STARTING;
static bool wifi_ready;
static bool time_synchronized;
static bool time_sync_task_started;
static bool websocket_started;
static uint32_t wifi_backoff_ms = WIFI_RECONNECT_INITIAL_MS;
static uint32_t websocket_backoff_ms = WEBSOCKET_RECONNECT_INITIAL_MS;
static uint8_t startup_server_attempts;
static bool startup_retries_exhausted;
static uint8_t short_lived_websocket_connections;
static int64_t connectivity_unhealthy_since_us;
static int64_t wifi_ready_since_us;
static int64_t protocol_ready_since_us;
static char wifi_ssid[33];
static char wifi_password[64];
static char server_url[257];
// ESP-IDF NVS strings have a 4000-byte limit including the terminating NUL.
static char server_certificate[4000];
static char timezone[65];

typedef struct {
    uint32_t generation;
    esp_websocket_client_handle_t client;
} websocket_generation_context_t;

typedef enum {
    CONNECTIVITY_COMMAND_RECONNECT,
    CONNECTIVITY_COMMAND_WIFI_RETRY,
    CONNECTIVITY_COMMAND_WIFI_CONNECT_TIMEOUT,
    CONNECTIVITY_COMMAND_WEBSOCKET_RETRY,
    CONNECTIVITY_COMMAND_WEBSOCKET_WELCOME_TIMEOUT,
    CONNECTIVITY_COMMAND_CONNECTIVITY_CHECK,
    CONNECTIVITY_COMMAND_WIFI_EVENT,
    CONNECTIVITY_COMMAND_WEBSOCKET_EVENT,
    CONNECTIVITY_COMMAND_PROTOCOL_READY,
    CONNECTIVITY_COMMAND_TIME_SYNC_FAILED,
} connectivity_command_type_t;

typedef struct {
    esp_websocket_client_handle_t client;
    uint32_t generation;
    int32_t event_id;
    esp_websocket_event_data_t event;
    uint8_t *data;
} websocket_event_command_t;

typedef struct {
    connectivity_command_type_t type;
    union {
        struct {
            esp_event_base_t base;
            int32_t event_id;
            int disconnect_reason;
        } wifi;
        websocket_event_command_t websocket;
    } data;
} connectivity_command_t;

static uint32_t websocket_generation;
static websocket_generation_context_t *websocket_context;

static void schedule_websocket_reconnect(void);
static void reconnect_websocket(void);
static void destroy_websocket(void);

static bool enqueue_connectivity_command(const connectivity_command_t *command)
{
    if (connectivity_commands == NULL || xQueueSend(connectivity_commands, command, 0) != pdPASS) {
        ESP_LOGW(TAG, "Connectivity command queue is full");
        return false;
    }
    return true;
}

static void reset_websocket_backoff_after_stable_connection(void)
{
    if (protocol_ready_since_us == 0) {
        return;
    }
    const int64_t connected_us = esp_timer_get_time() - protocol_ready_since_us;
    protocol_ready_since_us = 0;
    if (connected_us >= (int64_t)WEBSOCKET_STABLE_CONNECTION_MS * 1000LL) {
        websocket_backoff_ms = WEBSOCKET_RECONNECT_INITIAL_MS;
        short_lived_websocket_connections = 0;
    }
}

static void reset_wifi_backoff_after_stable_connection(void)
{
    if (wifi_ready_since_us == 0) {
        return;
    }
    const int64_t connected_us = esp_timer_get_time() - wifi_ready_since_us;
    wifi_ready_since_us = 0;
    if (connected_us >= (int64_t)WEBSOCKET_STABLE_CONNECTION_MS * 1000LL) {
        wifi_backoff_ms = WIFI_RECONNECT_INITIAL_MS;
    }
}

static uint32_t next_backoff(uint32_t current)
{
    return current >= RECONNECT_MAX_MS / 2U ? RECONNECT_MAX_MS : current * 2U;
}

static void reset_connection_state(void)
{
    satellite_protocol_reset();
    satellite_audio_reset();
}

static void set_status(satellite_connection_status_t status)
{
    connection_status = status;
    satellite_ui_post_connection_status(status);
}

bool satellite_connectivity_time_synchronized(void)
{
    return time_synchronized;
}

void satellite_connectivity_reconnect(void)
{
    ESP_LOGW(TAG, "Protocol requested WebSocket reconnect after a transmit failure");
    const connectivity_command_t command = {.type = CONNECTIVITY_COMMAND_RECONNECT};
    (void)enqueue_connectivity_command(&command);
}

static esp_err_t read_nvs_string(nvs_handle_t handle, const char *key, char *value, size_t value_size)
{
    size_t required_size = value_size;
    esp_err_t error = nvs_get_str(handle, key, value, &required_size);
    if (error == ESP_ERR_NVS_INVALID_LENGTH) {
        ESP_LOGE(TAG, "Provisioned %s value is too long", key);
    }
    return error;
}

static esp_err_t load_provisioning(void)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open("wifi", NVS_READONLY, &handle);
    if (error != ESP_OK) {
        return error;
    }
    error = read_nvs_string(handle, "ssid", wifi_ssid, sizeof(wifi_ssid));
    if (error == ESP_OK) {
        error = read_nvs_string(handle, "password", wifi_password, sizeof(wifi_password));
    }
    nvs_close(handle);
    if (error != ESP_OK) {
        return error;
    }

    error = nvs_open("server", NVS_READONLY, &handle);
    if (error != ESP_OK) {
        return error;
    }
    error = read_nvs_string(handle, "url", server_url, sizeof(server_url));
    if (error == ESP_OK) {
        error = read_nvs_string(handle, "certificate", server_certificate, sizeof(server_certificate));
    }
    nvs_close(handle);
    if (error != ESP_OK) {
        return error;
    }
    if (strncmp(server_url, "wss://", 6) != 0 || server_certificate[0] == '\0') {
        ESP_LOGE(TAG, "A secure server URL and public certificate are required");
        return ESP_ERR_INVALID_ARG;
    }

    error = nvs_open("device", NVS_READONLY, &handle);
    if (error != ESP_OK) {
        return error;
    }
    error = read_nvs_string(handle, "timezone", timezone, sizeof(timezone));
    nvs_close(handle);
    return error;
}

static const char *posix_timezone(const char *configured_timezone)
{
    if (strcmp(configured_timezone, "Australia/Sydney") == 0) {
        return "AEST-10AEDT,M10.1.0,M4.1.0/3";
    }
    return configured_timezone;
}

static void schedule_wifi_reconnect(void)
{
    if (esp_timer_is_active(wifi_reconnect_timer)) {
        return;
    }
    set_status(satellite_protocol_recovery_pending() ? SATELLITE_CONNECTION_RECOVERING
                                                      : SATELLITE_CONNECTION_WIFI_CONNECTING);
    ESP_LOGW(TAG, "Wi-Fi unavailable; retrying in %lu ms", (unsigned long)wifi_backoff_ms);
    ESP_ERROR_CHECK(esp_timer_start_once(wifi_reconnect_timer, wifi_backoff_ms * 1000ULL));
    wifi_backoff_ms = next_backoff(wifi_backoff_ms);
}

static void schedule_websocket_reconnect(void)
{
    if (!wifi_ready || websocket_started || esp_timer_is_active(websocket_reconnect_timer)) {
        return;
    }
    if (!startup_retries_exhausted && startup_server_attempts >= STARTUP_SERVER_ATTEMPTS) {
        startup_retries_exhausted = true;
        set_status(SATELLITE_CONNECTION_UNAVAILABLE);
        ESP_LOGW(TAG, "Server unavailable after %u startup attempts; rechecking in %u ms",
                 STARTUP_SERVER_ATTEMPTS, SERVER_RECHECK_MS);
        ESP_ERROR_CHECK(esp_timer_start_once(websocket_reconnect_timer, SERVER_RECHECK_MS * 1000ULL));
        return;
    }
    if (!satellite_protocol_recovery_pending()) {
        set_status(SATELLITE_CONNECTION_SERVER_CONNECTING);
    }
    const uint32_t retry_ms = startup_retries_exhausted ? SERVER_RECHECK_MS : websocket_backoff_ms;
    ESP_LOGW(TAG, "Secure server unavailable; retrying in %lu ms", (unsigned long)retry_ms);
    ESP_ERROR_CHECK(esp_timer_start_once(websocket_reconnect_timer, retry_ms * 1000ULL));
    if (!startup_retries_exhausted) {
        websocket_backoff_ms = next_backoff(websocket_backoff_ms);
    }
}

static void wifi_reconnect_timer_callback(void *argument)
{
    (void)argument;
    const connectivity_command_t command = {.type = CONNECTIVITY_COMMAND_WIFI_RETRY};
    (void)enqueue_connectivity_command(&command);
}

static void wifi_connect_timer_callback(void *argument)
{
    (void)argument;
    const connectivity_command_t command = {.type = CONNECTIVITY_COMMAND_WIFI_CONNECT_TIMEOUT};
    (void)enqueue_connectivity_command(&command);
}

static void websocket_welcome_timer_callback(void *argument)
{
    (void)argument;
    const connectivity_command_t command = {.type = CONNECTIVITY_COMMAND_WEBSOCKET_WELCOME_TIMEOUT};
    (void)enqueue_connectivity_command(&command);
}

static void websocket_reconnect_timer_callback(void *argument)
{
    (void)argument;
    const connectivity_command_t command = {.type = CONNECTIVITY_COMMAND_WEBSOCKET_RETRY};
    (void)enqueue_connectivity_command(&command);
}

static void reconnect_websocket(void)
{
    if (!websocket_started) {
        return;
    }
    const bool short_lived_connection = protocol_ready_since_us != 0 &&
                                         esp_timer_get_time() - protocol_ready_since_us <
                                             (int64_t)WEBSOCKET_STABLE_CONNECTION_MS * 1000LL;
    const int64_t connection_age_ms = protocol_ready_since_us == 0
                                          ? -1
                                          : (esp_timer_get_time() - protocol_ready_since_us) / 1000;
    ESP_LOGW(TAG, "Tearing down WebSocket: protocol_ready=%d age_ms=%lld short_lived=%d next_backoff_ms=%lu",
             satellite_protocol_ready(), (long long)connection_age_ms, short_lived_connection,
             (unsigned long)websocket_backoff_ms);
    reset_websocket_backoff_after_stable_connection();
    if (short_lived_connection && short_lived_websocket_connections < UINT8_MAX) {
        short_lived_websocket_connections++;
    }
    if (esp_timer_is_active(websocket_welcome_timer)) {
        ESP_ERROR_CHECK(esp_timer_stop(websocket_welcome_timer));
    }
    satellite_protocol_disconnected();
    set_status(satellite_protocol_recovery_pending() ? SATELLITE_CONNECTION_RECOVERING
                                                      : SATELLITE_CONNECTION_SERVER_CONNECTING);
    destroy_websocket();
    if (wifi_ready) {
        if (short_lived_websocket_connections >= WEBSOCKET_SHORT_LIVED_CONNECTION_LIMIT) {
            short_lived_websocket_connections = 0;
            wifi_ready = false;
            ESP_LOGW(TAG, "Server connections repeatedly failed before stabilization; reassociating Wi-Fi");
            const esp_err_t error = esp_wifi_disconnect();
            if (error != ESP_OK && error != ESP_ERR_WIFI_NOT_CONNECT) {
                ESP_LOGW(TAG, "Unable to disconnect Wi-Fi before reassociation (%s)", esp_err_to_name(error));
            }
            schedule_wifi_reconnect();
            return;
        }
        schedule_websocket_reconnect();
    }
}

static void connectivity_check_timer_callback(void *argument)
{
    (void)argument;
    const connectivity_command_t command = {.type = CONNECTIVITY_COMMAND_CONNECTIVITY_CHECK};
    (void)enqueue_connectivity_command(&command);
}

static void time_sync_task(void *argument)
{
    (void)argument;
    const esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    if (esp_netif_sntp_init(&sntp_config) != ESP_OK) {
        ESP_LOGE(TAG, "SNTP initialization failed; secure connection is unavailable");
        const connectivity_command_t command = {.type = CONNECTIVITY_COMMAND_TIME_SYNC_FAILED};
        (void)enqueue_connectivity_command(&command);
        vTaskDelete(NULL);
        return;
    }
    for (;;) {
        if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(15000)) == ESP_OK) {
            time_synchronized = true;
            ESP_LOGI(TAG, "Clock synchronized");
            break;
        } else {
            ESP_LOGW(TAG, "Waiting for clock synchronization");
        }
    }
    vTaskDelete(NULL);
}

static void websocket_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)base;
    const websocket_generation_context_t *context = handler_args;
    if (context == NULL) {
        return;
    }

    if (event_id == WEBSOCKET_EVENT_DATA) {
        if (context->generation != websocket_generation || context->client != websocket) {
            return;
        }
        satellite_protocol_handle_data(context->client, event_data);
        if (satellite_protocol_ready()) {
            const connectivity_command_t ready = {.type = CONNECTIVITY_COMMAND_PROTOCOL_READY};
            (void)enqueue_connectivity_command(&ready);
        }
        return;
    }

    connectivity_command_t command = {
        .type = CONNECTIVITY_COMMAND_WEBSOCKET_EVENT,
        .data.websocket = {
            .client = context->client,
            .generation = context->generation,
            .event_id = event_id,
        },
    };
    if (event_data != NULL) {
        command.data.websocket.event = *(const esp_websocket_event_data_t *)event_data;
    }
    (void)enqueue_connectivity_command(&command);
}

static void wifi_event_handler(void *argument, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)argument;
    connectivity_command_t command = {
        .type = CONNECTIVITY_COMMAND_WIFI_EVENT,
        .data.wifi = {.base = base, .event_id = event_id},
    };
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED && event_data != NULL) {
        command.data.wifi.disconnect_reason = ((const wifi_event_sta_disconnected_t *)event_data)->reason;
    }
    (void)enqueue_connectivity_command(&command);
}

static void destroy_websocket(void)
{
    if (websocket == NULL) {
        return;
    }
    if (websocket_started) {
        ESP_LOGI(TAG, "Stopping WebSocket client");
        const esp_err_t error = esp_websocket_client_stop(websocket);
        if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "Unable to stop WebSocket client (%s)", esp_err_to_name(error));
        }
        ESP_LOGI(TAG, "WebSocket client stopped");
    }
    const esp_err_t error = esp_websocket_client_destroy(websocket);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Unable to destroy WebSocket client (%s)", esp_err_to_name(error));
    }
    websocket = NULL;
    websocket_started = false;
    free(websocket_context);
    websocket_context = NULL;
}

static void start_websocket(void)
{
    if (!wifi_ready || websocket_started) {
        return;
    }
    if (!time_synchronized) {
        ESP_LOGW(TAG, "Waiting for clock synchronization before verifying the server certificate");
        schedule_websocket_reconnect();
        return;
    }
    if (!startup_retries_exhausted) {
        startup_server_attempts++;
    }

    websocket_context = calloc(1, sizeof(*websocket_context));
    if (websocket_context == NULL) {
        set_status(SATELLITE_CONNECTION_ERROR);
        schedule_websocket_reconnect();
        return;
    }
    websocket_context->generation = ++websocket_generation;
    const esp_websocket_client_config_t config = {
        .uri = server_url,
        .cert_pem = server_certificate,
        .task_stack = WEBSOCKET_TASK_STACK_BYTES,
        .disable_auto_reconnect = true,
        .enable_close_reconnect = false,
        .network_timeout_ms = WEBSOCKET_NETWORK_TIMEOUT_MS,
        .ping_interval_sec = 10,
        .pingpong_timeout_sec = 15,
        .disable_pingpong_discon = true,
    };
    websocket = esp_websocket_client_init(&config);
    if (websocket == NULL) {
        free(websocket_context);
        websocket_context = NULL;
        set_status(SATELLITE_CONNECTION_ERROR);
        schedule_websocket_reconnect();
        return;
    }
    websocket_context->client = websocket;
    if (esp_websocket_register_events(websocket, WEBSOCKET_EVENT_ANY, websocket_event_handler, websocket_context) != ESP_OK ||
        esp_websocket_client_start(websocket) != ESP_OK) {
        set_status(SATELLITE_CONNECTION_ERROR);
        destroy_websocket();
        schedule_websocket_reconnect();
        return;
    }
    websocket_started = true;
    set_status(satellite_protocol_recovery_pending() ? SATELLITE_CONNECTION_RECOVERING
                                                      : SATELLITE_CONNECTION_SERVER_CONNECTING);
    ESP_LOGI(TAG, "Connecting to local secure server");
}

static void handle_websocket_event(const connectivity_command_t *command)
{
    const websocket_event_command_t *websocket_event = &command->data.websocket;
    if (websocket_event->generation != websocket_generation || websocket_event->client != websocket) {
        return;
    }
    const esp_websocket_event_data_t *event = &websocket_event->event;
    if (websocket_event->event_id == WEBSOCKET_EVENT_CONNECTED) {
        if (satellite_protocol_connected(websocket)) {
            ESP_ERROR_CHECK(esp_timer_start_once(websocket_welcome_timer,
                                                  WEBSOCKET_WELCOME_TIMEOUT_MS * 1000ULL));
            startup_server_attempts = 0;
            startup_retries_exhausted = false;
            set_status(SATELLITE_CONNECTION_SERVER_CONNECTING);
            ESP_LOGI(TAG, "Secure server connection established; awaiting protocol welcome");
        } else {
            set_status(SATELLITE_CONNECTION_ERROR);
            reconnect_websocket();
        }
    } else if (websocket_event->event_id == WEBSOCKET_EVENT_ERROR) {
        if (!satellite_protocol_ready()) {
            set_status(SATELLITE_CONNECTION_ERROR);
        }
        if (event == NULL || event->error_handle.error_type == WEBSOCKET_ERROR_TYPE_NONE) {
            ESP_LOGW(TAG, "Secure server transport closed without error details");
        } else {
            ESP_LOGW(TAG, "Secure server error: type=%d tls=%s stack=%d verify_flags=0x%x handshake=%d socket_errno=%d",
                     event->error_handle.error_type, esp_err_to_name(event->error_handle.esp_tls_last_esp_err),
                     event->error_handle.esp_tls_stack_err, event->error_handle.esp_tls_cert_verify_flags,
                     event->error_handle.esp_ws_handshake_status_code, event->error_handle.esp_transport_sock_errno);
        }
        reconnect_websocket();
    } else if (websocket_event->event_id == WEBSOCKET_EVENT_DISCONNECTED ||
                websocket_event->event_id == WEBSOCKET_EVENT_CLOSED) {
        ESP_LOGW(TAG, "WebSocket terminal event: %s", websocket_event->event_id == WEBSOCKET_EVENT_DISCONNECTED
                                                          ? "disconnected"
                                                          : "closed");
        if (event != NULL && event->close_status_code != 0) {
            ESP_LOGW(TAG, "Secure server closed WebSocket with status %d", event->close_status_code);
        }
        reconnect_websocket();
    }
}

static void connectivity_task(void *argument)
{
    (void)argument;
    for (;;) {
        connectivity_command_t command;
        if (xQueueReceive(connectivity_commands, &command, portMAX_DELAY) != pdPASS) {
            continue;
        }
        if (command.type == CONNECTIVITY_COMMAND_RECONNECT) {
            reconnect_websocket();
        } else if (command.type == CONNECTIVITY_COMMAND_WIFI_RETRY) {
            if (esp_wifi_connect() != ESP_OK) {
                schedule_wifi_reconnect();
            } else {
                ESP_ERROR_CHECK(esp_timer_start_once(wifi_connect_timer, WIFI_CONNECT_TIMEOUT_MS * 1000ULL));
            }
        } else if (command.type == CONNECTIVITY_COMMAND_WIFI_CONNECT_TIMEOUT) {
            if (!wifi_ready) {
                ESP_LOGW(TAG, "Wi-Fi connection timed out; retrying");
                ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_disconnect());
                schedule_wifi_reconnect();
            }
        } else if (command.type == CONNECTIVITY_COMMAND_WEBSOCKET_RETRY) {
            start_websocket();
        } else if (command.type == CONNECTIVITY_COMMAND_WEBSOCKET_WELCOME_TIMEOUT) {
            if (websocket_started && !satellite_protocol_ready()) {
                ESP_LOGW(TAG, "Server protocol welcome timed out; reconnecting");
                reconnect_websocket();
            }
        } else if (command.type == CONNECTIVITY_COMMAND_CONNECTIVITY_CHECK) {
            if (wifi_ready && satellite_protocol_ready()) {
                const int64_t last_activity = satellite_protocol_last_server_activity_us();
                if (last_activity != 0 && esp_timer_get_time() - last_activity >= (int64_t)SERVER_INBOUND_STALL_MS * 1000LL) {
                    ESP_LOGW(TAG, "Server traffic stalled for %u ms; reconnecting", SERVER_INBOUND_STALL_MS);
                    reconnect_websocket();
                } else {
                    connectivity_unhealthy_since_us = 0;
                }
            } else if (connectivity_unhealthy_since_us == 0) {
                connectivity_unhealthy_since_us = esp_timer_get_time();
            } else if (esp_timer_get_time() - connectivity_unhealthy_since_us >=
                       (int64_t)CONNECTIVITY_STALL_RESET_MS * 1000LL) {
                ESP_LOGE(TAG, "Connectivity did not recover within %u ms; restarting", CONNECTIVITY_STALL_RESET_MS);
                esp_restart();
            }
        } else if (command.type == CONNECTIVITY_COMMAND_WIFI_EVENT) {
            if (command.data.wifi.base == WIFI_EVENT && command.data.wifi.event_id == WIFI_EVENT_STA_START) {
                schedule_wifi_reconnect();
            } else if ((command.data.wifi.base == WIFI_EVENT && command.data.wifi.event_id == WIFI_EVENT_STA_DISCONNECTED) ||
                       (command.data.wifi.base == IP_EVENT && command.data.wifi.event_id == IP_EVENT_STA_LOST_IP)) {
                if (command.data.wifi.base == WIFI_EVENT) {
                    ESP_LOGW(TAG, "Wi-Fi disconnected (reason=%d)", command.data.wifi.disconnect_reason);
                } else {
                    ESP_LOGW(TAG, "Wi-Fi lost its IP address");
                }
                reset_wifi_backoff_after_stable_connection();
                reset_websocket_backoff_after_stable_connection();
                wifi_ready = false;
                if (esp_timer_is_active(wifi_connect_timer)) {
                    ESP_ERROR_CHECK(esp_timer_stop(wifi_connect_timer));
                }
                if (websocket_started) {
                    satellite_protocol_disconnected();
                    destroy_websocket();
                } else {
                    reset_connection_state();
                }
                schedule_wifi_reconnect();
            } else if (command.data.wifi.base == IP_EVENT && command.data.wifi.event_id == IP_EVENT_STA_GOT_IP) {
                wifi_ready = true;
                if (esp_timer_is_active(wifi_connect_timer)) {
                    ESP_ERROR_CHECK(esp_timer_stop(wifi_connect_timer));
                }
                wifi_ready_since_us = esp_timer_get_time();
                set_status(satellite_protocol_recovery_pending() ? SATELLITE_CONNECTION_RECOVERING
                                                                  : SATELLITE_CONNECTION_WIFI_READY);
                ESP_LOGI(TAG, "Wi-Fi connected");
                schedule_websocket_reconnect();
                if (!time_sync_task_started) {
                    if (xTaskCreate(time_sync_task, "time_sync", 4096, NULL, 4, NULL) == pdPASS) {
                        time_sync_task_started = true;
                    } else {
                        ESP_LOGE(TAG, "Clock synchronization task creation failed");
                        set_status(SATELLITE_CONNECTION_ERROR);
                    }
                }
            }
        } else if (command.type == CONNECTIVITY_COMMAND_WEBSOCKET_EVENT) {
            handle_websocket_event(&command);
        } else if (command.type == CONNECTIVITY_COMMAND_PROTOCOL_READY) {
            if (websocket_started && satellite_protocol_ready() && connection_status != SATELLITE_CONNECTION_CONNECTED) {
                if (esp_timer_is_active(websocket_welcome_timer)) {
                    ESP_ERROR_CHECK(esp_timer_stop(websocket_welcome_timer));
                }
                protocol_ready_since_us = esp_timer_get_time();
                set_status(SATELLITE_CONNECTION_CONNECTED);
                ESP_LOGI(TAG, "Satellite protocol ready");
            }
        } else if (command.type == CONNECTIVITY_COMMAND_TIME_SYNC_FAILED) {
            set_status(SATELLITE_CONNECTION_ERROR);
        }
    }
}

esp_err_t satellite_connectivity_start(void)
{
    esp_err_t error = load_provisioning();
    if (error != ESP_OK) {
        ESP_LOGE(TAG, "Provisioning bundle is missing or invalid (%s)", esp_err_to_name(error));
        return error;
    }
    if (setenv("TZ", posix_timezone(timezone), 1) != 0) {
        ESP_LOGE(TAG, "Unable to set device timezone");
        return ESP_FAIL;
    }
    tzset();
    ESP_RETURN_ON_ERROR(satellite_protocol_init(), TAG, "Protocol provisioning initialization failed");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "Network stack initialization failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "Event loop initialization failed");
    if (esp_netif_create_default_wifi_sta() == NULL) {
        return ESP_FAIL;
    }
    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&wifi_init_config), TAG, "Wi-Fi initialization failed");
    // The always-on audio uplink benefits from prompt radio wakeups after a
    // mesh handoff; this mains-oriented satellite favors link stability over battery life.
    ESP_RETURN_ON_ERROR(esp_wifi_set_ps(WIFI_PS_NONE), TAG, "Wi-Fi power-save configuration failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL), TAG,
                        "Wi-Fi event registration failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL), TAG,
                        "IP event registration failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, wifi_event_handler, NULL), TAG,
                        "IP loss event registration failed");

    wifi_config_t wifi_config = {0};
    memcpy(wifi_config.sta.ssid, wifi_ssid,
           strnlen(wifi_ssid, sizeof(wifi_config.sta.ssid)));
    memcpy(wifi_config.sta.password, wifi_password,
           strnlen(wifi_password, sizeof(wifi_config.sta.password)));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "Wi-Fi mode configuration failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "Wi-Fi configuration failed");

    const esp_timer_create_args_t wifi_timer_config = {
        .callback = wifi_reconnect_timer_callback,
        .name = "wifi_retry",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&wifi_timer_config, &wifi_reconnect_timer), TAG,
                        "Wi-Fi retry timer creation failed");
    const esp_timer_create_args_t websocket_timer_config = {
        .callback = websocket_reconnect_timer_callback,
        .name = "ws_retry",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&websocket_timer_config, &websocket_reconnect_timer), TAG,
                        "WebSocket retry timer creation failed");
    const esp_timer_create_args_t connectivity_check_timer_config = {
        .callback = connectivity_check_timer_callback,
        .name = "conn_check",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&connectivity_check_timer_config, &connectivity_check_timer), TAG,
                        "Connectivity check timer creation failed");
    const esp_timer_create_args_t wifi_connect_timer_config = {
        .callback = wifi_connect_timer_callback,
        .name = "wifi_connect",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&wifi_connect_timer_config, &wifi_connect_timer), TAG,
                        "Wi-Fi connection timer creation failed");
    const esp_timer_create_args_t websocket_welcome_timer_config = {
        .callback = websocket_welcome_timer_callback,
        .name = "ws_welcome",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&websocket_welcome_timer_config, &websocket_welcome_timer), TAG,
                        "WebSocket welcome timer creation failed");

    connectivity_commands = xQueueCreate(CONNECTIVITY_COMMAND_QUEUE_LENGTH, sizeof(connectivity_command_t));
    if (connectivity_commands == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(connectivity_task, "connectivity", CONNECTIVITY_TASK_STACK_BYTES, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(connectivity_check_timer,
                                                  (uint64_t)CONNECTIVITY_CHECK_MS * 1000ULL),
                        TAG, "Connectivity check timer start failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi start failed");
    return ESP_OK;
}
