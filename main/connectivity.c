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
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "protocol.h"
#include "ui.h"

#define WIFI_RECONNECT_INITIAL_MS 1000U
#define WEBSOCKET_RECONNECT_INITIAL_MS 1000U
#define RECONNECT_MAX_MS 60000U
#define STARTUP_SERVER_ATTEMPTS 3U
#define SERVER_RECHECK_MS 60000U
#define WEBSOCKET_OPCODE_CLOSE 0x8
#define WEBSOCKET_TASK_STACK_BYTES 8192
#define WEBSOCKET_NETWORK_TIMEOUT_MS 60000

static const char *TAG = "connectivity";

static esp_websocket_client_handle_t websocket;
static esp_timer_handle_t wifi_reconnect_timer;
static esp_timer_handle_t websocket_reconnect_timer;
static satellite_connection_status_t connection_status = SATELLITE_CONNECTION_STARTING;
static bool wifi_ready;
static bool time_synchronized;
static bool time_sync_task_started;
static bool websocket_started;
static uint32_t wifi_backoff_ms = WIFI_RECONNECT_INITIAL_MS;
static uint32_t websocket_backoff_ms = WEBSOCKET_RECONNECT_INITIAL_MS;
static uint8_t startup_server_attempts;
static bool startup_retries_exhausted;
static char wifi_ssid[33];
static char wifi_password[64];
static char server_url[257];
static char timezone[65];

/* These fields and the reset point keep reconnects from inheriting a stale turn. */
static char session_id[65];
static char active_turn_id[65];

static void schedule_websocket_reconnect(void);

static uint32_t next_backoff(uint32_t current)
{
    return current >= RECONNECT_MAX_MS / 2U ? RECONNECT_MAX_MS : current * 2U;
}

static void reset_connection_state(void)
{
    session_id[0] = '\0';
    active_turn_id[0] = '\0';
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
    if (!websocket_started) {
        return;
    }
    websocket_started = false;
    reset_connection_state();
    set_status(SATELLITE_CONNECTION_SERVER_CONNECTING);
    if (websocket != NULL) {
        (void)esp_websocket_client_stop(websocket);
    }
    if (wifi_ready) {
        schedule_websocket_reconnect();
    }
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
    nvs_close(handle);
    if (error != ESP_OK) {
        return error;
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
    set_status(SATELLITE_CONNECTION_WIFI_CONNECTING);
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
    set_status(SATELLITE_CONNECTION_SERVER_CONNECTING);
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
    if (esp_wifi_connect() != ESP_OK) {
        schedule_wifi_reconnect();
    }
}

static void websocket_reconnect_timer_callback(void *argument)
{
    (void)argument;
    if (!wifi_ready || websocket_started) {
        return;
    }
    reset_connection_state();
    if (!startup_retries_exhausted) {
        startup_server_attempts++;
    }
    if (esp_websocket_client_start(websocket) == ESP_OK) {
        websocket_started = true;
        set_status(SATELLITE_CONNECTION_SERVER_CONNECTING);
        ESP_LOGI(TAG, "Connecting to local secure server");
    } else {
        set_status(SATELLITE_CONNECTION_ERROR);
        schedule_websocket_reconnect();
    }
}

static void time_sync_task(void *argument)
{
    (void)argument;
    const esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    if (esp_netif_sntp_init(&sntp_config) != ESP_OK) {
        ESP_LOGE(TAG, "SNTP initialization failed; secure connection is unavailable");
        set_status(SATELLITE_CONNECTION_ERROR);
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
    (void)handler_args;
    (void)base;
    esp_websocket_event_data_t *event = event_data;

    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        websocket_backoff_ms = WEBSOCKET_RECONNECT_INITIAL_MS;
        if (satellite_protocol_connected(websocket)) {
            startup_server_attempts = 0;
            startup_retries_exhausted = false;
            set_status(SATELLITE_CONNECTION_SERVER_CONNECTING);
            ESP_LOGI(TAG, "Secure server connection established; awaiting protocol welcome");
        } else {
            set_status(SATELLITE_CONNECTION_ERROR);
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_websocket_client_stop(websocket));
        }
    } else if (event_id == WEBSOCKET_EVENT_DATA) {
        if (event != NULL && event->op_code == WEBSOCKET_OPCODE_CLOSE) {
            ESP_LOGW(TAG, "Secure server sent WebSocket close with status %d", event->close_status_code);
        }
        satellite_protocol_handle_data(websocket, event);
        if (satellite_protocol_ready() && connection_status != SATELLITE_CONNECTION_CONNECTED) {
            set_status(SATELLITE_CONNECTION_CONNECTED);
            ESP_LOGI(TAG, "Satellite protocol ready");
        }
    } else if (event_id == WEBSOCKET_EVENT_ERROR) {
        if (!satellite_protocol_ready()) {
            set_status(SATELLITE_CONNECTION_ERROR);
        }
        if (event != NULL) {
            if (event->error_handle.error_type == WEBSOCKET_ERROR_TYPE_NONE) {
                ESP_LOGW(TAG, "Secure server transport closed without error details");
            } else {
                ESP_LOGW(TAG,
                         "Secure server error: type=%d tls=%s stack=%d verify_flags=0x%x handshake=%d socket_errno=%d",
                         event->error_handle.error_type, esp_err_to_name(event->error_handle.esp_tls_last_esp_err),
                         event->error_handle.esp_tls_stack_err, event->error_handle.esp_tls_cert_verify_flags,
                         event->error_handle.esp_ws_handshake_status_code,
                         event->error_handle.esp_transport_sock_errno);
            }
        } else {
            ESP_LOGW(TAG, "Secure server transport closed unexpectedly");
        }
        satellite_connectivity_reconnect();
    } else if (event_id == WEBSOCKET_EVENT_DISCONNECTED) {
        if (event != NULL && event->close_status_code != 0) {
            ESP_LOGW(TAG, "Secure server closed WebSocket with status %d", event->close_status_code);
        }
        satellite_connectivity_reconnect();
    }
}

static void wifi_event_handler(void *argument, esp_event_base_t base, int32_t event_id, void *event_data)
{
    (void)argument;
    (void)event_data;
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        schedule_wifi_reconnect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_ready = false;
        reset_connection_state();
        if (websocket_started) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_websocket_client_stop(websocket));
            websocket_started = false;
        }
        schedule_wifi_reconnect();
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        wifi_ready = true;
        wifi_backoff_ms = WIFI_RECONNECT_INITIAL_MS;
        set_status(SATELLITE_CONNECTION_WIFI_READY);
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
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL), TAG,
                        "Wi-Fi event registration failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL), TAG,
                        "IP event registration failed");

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

    const esp_websocket_client_config_t websocket_config = {
        .uri = server_url,
        .task_stack = WEBSOCKET_TASK_STACK_BYTES,
        .disable_auto_reconnect = true,
        .enable_close_reconnect = false,
        .network_timeout_ms = WEBSOCKET_NETWORK_TIMEOUT_MS,
        .ping_interval_sec = 10,
        .pingpong_timeout_sec = 15,
    };
    websocket = esp_websocket_client_init(&websocket_config);
    if (websocket == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(esp_websocket_register_events(websocket, WEBSOCKET_EVENT_ANY, websocket_event_handler, NULL), TAG,
                        "WebSocket event registration failed");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi start failed");
    return ESP_OK;
}
