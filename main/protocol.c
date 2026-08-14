#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "cJSON.h"
#include "audio.h"
#include "connectivity.h"
#include "esp_check.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "protocol.h"
#include "ui.h"

#define CONTROL_MESSAGE_MAX_BYTES 2048
#define DOWNLINK_MAX_BYTES 4096
#define TURN_ID_MAX_BYTES 64
#define HEALTH_REQUEST_ID_MAX_BYTES 64
#define IDLE_DRAIN_MS 250
#define IDLE_RESPONSE_START_MS 5000
#define WEBSOCKET_SEND_TIMEOUT_MS 1000
#define TTS_RECOVERY_WINDOW_MS 2000

#define WEBSOCKET_OPCODE_TEXT 0x1
#define WEBSOCKET_OPCODE_BINARY 0x2
#define WEBSOCKET_OPCODE_CLOSE 0x8
#define WEBSOCKET_OPCODE_PING 0x9
#define WEBSOCKET_OPCODE_PONG 0xA

static const char *TAG = "protocol";

static char device_id[65];
static char device_name[65];
static char device_token[257];
static char active_turn_id[TURN_ID_MAX_BYTES + 1];
static char session_id[TURN_ID_MAX_BYTES + 1];
static esp_websocket_client_handle_t protocol_websocket;
static esp_timer_handle_t idle_drain_timer;
static esp_timer_handle_t recovery_timer;
static SemaphoreHandle_t websocket_send_lock;
static bool protocol_ready;
static bool stop_pending;
static bool idle_pending;
static bool barge_in_turn_pending;
static satellite_assistant_state_t assistant_state = SATELLITE_ASSISTANT_IDLE;
static uint32_t uplink_frames_sent;
static int64_t last_uplink_send_us;
static int16_t *downlink_payload_samples;
static size_t downlink_payload_bytes;
static size_t downlink_payload_length;
static bool tts_audio_pending;
static size_t tts_audio_bytes;
static uint32_t tts_audio_sequence;
static uint32_t expected_downlink_sequence;
static uint32_t last_played_sequence;
static bool last_played_sequence_valid;
static bool recovery_pending;

static void clear_tts_audio_pending(void)
{
    tts_audio_pending = false;
    tts_audio_bytes = 0;
}

static void discard_tts_audio_pending(void)
{
    if (tts_audio_pending) {
        expected_downlink_sequence = tts_audio_sequence;
    }
    clear_tts_audio_pending();
}

static void clear_recovery_state(void)
{
    recovery_pending = false;
    session_id[0] = '\0';
    active_turn_id[0] = '\0';
    expected_downlink_sequence = 0;
    last_played_sequence = 0;
    last_played_sequence_valid = false;
    clear_tts_audio_pending();
    if (recovery_timer != NULL && esp_timer_is_active(recovery_timer)) {
        esp_timer_stop(recovery_timer);
    }
}

static void recovery_timer_callback(void *argument)
{
    (void)argument;
    if (!recovery_pending) {
        return;
    }
    ESP_LOGW(TAG, "TTS reconnect recovery window expired");
    clear_recovery_state();
    assistant_state = SATELLITE_ASSISTANT_IDLE;
    satellite_audio_reset();
    satellite_ui_post_assistant_state(assistant_state);
    satellite_ui_post_connection_status(SATELLITE_CONNECTION_SERVER_CONNECTING);
}

static bool json_string(const cJSON *object, const char *name, const char **value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }
    *value = item->valuestring;
    return true;
}

static bool json_integer(const cJSON *object, const char *name, int *value)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item) || item->valuedouble != (double)item->valueint) {
        return false;
    }
    *value = item->valueint;
    return true;
}

static bool json_matches_string(const cJSON *object, const char *name, const char *expected)
{
    const char *value;
    return json_string(object, name, &value) && strcmp(value, expected) == 0;
}

static esp_err_t read_nvs_string(const char *namespace, const char *key, char *value, size_t value_size)
{
    nvs_handle_t handle;
    esp_err_t error = nvs_open(namespace, NVS_READONLY, &handle);
    if (error != ESP_OK) {
        return error;
    }
    size_t required_size = value_size;
    error = nvs_get_str(handle, key, value, &required_size);
    nvs_close(handle);
    return error;
}

static void protocol_violation(esp_websocket_client_handle_t websocket)
{
    (void)websocket;
    ESP_LOGW(TAG, "Discarding invalid server protocol message");
}

static void reconnect_after_send_failure(void)
{
    ESP_LOGW(TAG, "WebSocket send failed; reconnecting");
    satellite_connectivity_reconnect();
}

static bool respond_to_health_request(const char *request_id)
{
    char health[320];

    if (strnlen(request_id, HEALTH_REQUEST_ID_MAX_BYTES + 1) > HEALTH_REQUEST_ID_MAX_BYTES) {
        return false;
    }
    const satellite_audio_metrics_t metrics = satellite_audio_metrics();
    const int health_length = snprintf(
        health, sizeof(health),
        "{\"type\":\"satellite.health_response\",\"id\":\"%s\",\"dropped_uplink_frames\":%lu,"
        "\"dropped_downlink_frames\":%lu,\"capture_overruns\":%lu,\"playback_underruns\":%lu}",
        request_id, (unsigned long)metrics.dropped_uplink_frames, (unsigned long)metrics.dropped_downlink_frames,
        (unsigned long)metrics.capture_overruns, (unsigned long)metrics.playback_underruns);
    if (health_length < 0 || health_length >= (int)sizeof(health)) {
        return false;
    } else if (xSemaphoreTake(websocket_send_lock, pdMS_TO_TICKS(1000)) != pdPASS) {
        return false;
    } else {
        const bool sent = protocol_ready && protocol_websocket != NULL &&
                          esp_websocket_client_send_text(protocol_websocket, health, health_length,
                                                         pdMS_TO_TICKS(WEBSOCKET_SEND_TIMEOUT_MS)) >= 0;
        xSemaphoreGive(websocket_send_lock);
        return sent;
    }
}

static void complete_idle_state(void)
{
    const bool was_speaking = assistant_state == SATELLITE_ASSISTANT_SPEAKING;

    clear_recovery_state();
    stop_pending = false;
    barge_in_turn_pending = false;
    assistant_state = SATELLITE_ASSISTANT_IDLE;
    if (was_speaking) {
        satellite_audio_finish_playback();
    }
    satellite_ui_post_assistant_state(assistant_state);
}

static void schedule_idle_completion(uint32_t delay_ms)
{
    if (idle_drain_timer == NULL) {
        return;
    }
    const esp_err_t error = esp_timer_is_active(idle_drain_timer)
                                ? esp_timer_restart(idle_drain_timer, (uint64_t)delay_ms * 1000ULL)
                                : esp_timer_start_once(idle_drain_timer, (uint64_t)delay_ms * 1000ULL);
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Unable to schedule assistant idle state");
    }
}

static void idle_drain_timer_callback(void *argument)
{
    (void)argument;
    if (!idle_pending) {
        return;
    }
    if (satellite_audio_playback_pending()) {
        schedule_idle_completion(IDLE_DRAIN_MS);
        return;
    }
    idle_pending = false;
    complete_idle_state();
}

static bool validate_audio_contract(const cJSON *welcome)
{
    const cJSON *audio = cJSON_GetObjectItemCaseSensitive(welcome, "audio");
    const cJSON *uplink = audio == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(audio, "uplink");
    const cJSON *downlink = audio == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(audio, "downlink");
    int value;

    return cJSON_IsObject(audio) && cJSON_IsObject(uplink) && cJSON_IsObject(downlink) &&
           json_matches_string(uplink, "encoding", "pcm_s16le") &&
           json_integer(uplink, "sample_rate_hz", &value) && value == 16000 &&
           json_integer(uplink, "channels", &value) && value == 1 &&
           json_integer(uplink, "frame_duration_ms", &value) && value == 20 &&
           json_matches_string(downlink, "encoding", "pcm_s16le") &&
           json_integer(downlink, "sample_rate_hz", &value) && value == 16000 &&
           json_integer(downlink, "channels", &value) && value == 1;
}

static bool validate_welcome(const cJSON *welcome)
{
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(welcome, "protocol");
    const char *session;
    int major;
    int minor;

    if (!json_string(welcome, "session_id", &session) || session[0] == '\0' ||
        strnlen(session, TURN_ID_MAX_BYTES + 1) > TURN_ID_MAX_BYTES || !cJSON_IsObject(version) ||
         !json_integer(version, "major", &major) || major != 2 ||
         !json_integer(version, "minor", &minor) || minor != 3 ||
        !validate_audio_contract(welcome)) {
        return false;
    }

    strlcpy(session_id, session, sizeof(session_id));
    protocol_ready = true;
    if (recovery_pending) {
        expected_downlink_sequence = last_played_sequence_valid ? last_played_sequence + 1 : 0;
        satellite_audio_begin_playback();
        recovery_pending = false;
        if (recovery_timer != NULL && esp_timer_is_active(recovery_timer)) {
            esp_timer_stop(recovery_timer);
        }
    }
    if (satellite_audio_enable_aec() != ESP_OK) {
        ESP_LOGE(TAG, "AEC activation failed; remaining in half-duplex mode");
    }
    return true;
}

static void reset_downlink_payload(void)
{
    downlink_payload_bytes = 0;
    downlink_payload_length = 0;
}

static bool validate_tts_audio(const cJSON *message)
{
    const char *turn_id;
    int sequence;
    int bytes;

    if (!json_string(message, "turn_id", &turn_id) || !json_integer(message, "seq", &sequence) ||
        !json_integer(message, "bytes", &bytes) || sequence < 0 || bytes <= 0 ||
        bytes > DOWNLINK_MAX_BYTES || (bytes % sizeof(int16_t)) != 0 ||
        assistant_state != SATELLITE_ASSISTANT_SPEAKING || strcmp(turn_id, active_turn_id) != 0 ||
        (uint32_t)sequence != expected_downlink_sequence) {
        return false;
    }
    tts_audio_pending = true;
    tts_audio_bytes = (size_t)bytes;
    tts_audio_sequence = (uint32_t)sequence;
    expected_downlink_sequence++;
    return true;
}

static bool assistant_accepts_downlink(void)
{
    return assistant_state == SATELLITE_ASSISTANT_THINKING ||
           assistant_state == SATELLITE_ASSISTANT_SPEAKING;
}

static void handle_binary_message(esp_websocket_client_handle_t websocket,
                                  const esp_websocket_event_data_t *event)
{
    if (!protocol_ready || event->data_len <= 0 || event->payload_len <= 0 ||
        event->payload_len > DOWNLINK_MAX_BYTES || (event->payload_len % sizeof(int16_t)) != 0) {
        protocol_violation(websocket);
        discard_tts_audio_pending();
        reset_downlink_payload();
        return;
    }
    if (!tts_audio_pending) {
        protocol_violation(websocket);
        reset_downlink_payload();
        return;
    }

    const size_t payload_offset = (size_t)event->payload_offset;
    const size_t data_len = (size_t)event->data_len;
    const size_t payload_len = (size_t)event->payload_len;
    if (payload_offset > payload_len || data_len > payload_len - payload_offset) {
        protocol_violation(websocket);
        discard_tts_audio_pending();
        reset_downlink_payload();
        return;
    }

    if (tts_audio_pending && payload_len != tts_audio_bytes) {
        protocol_violation(websocket);
        discard_tts_audio_pending();
        reset_downlink_payload();
        return;
    }
    if (payload_len == data_len && payload_offset == 0) {
        if (assistant_accepts_downlink()) {
            if (satellite_audio_enqueue_downlink((const int16_t *)event->data_ptr,
                                                 data_len / sizeof(int16_t), tts_audio_pending,
                                                 tts_audio_sequence) &&
                assistant_state == SATELLITE_ASSISTANT_SPEAKING) {
                satellite_ui_post_playback_activity();
            }
            if (idle_pending) {
                schedule_idle_completion(IDLE_DRAIN_MS);
            }
        }
        clear_tts_audio_pending();
        reset_downlink_payload();
        return;
    }

    if (payload_offset == 0) {
        downlink_payload_length = payload_len;
        downlink_payload_bytes = 0;
    }
    if (downlink_payload_length != payload_len || downlink_payload_bytes != payload_offset) {
        protocol_violation(websocket);
        discard_tts_audio_pending();
        reset_downlink_payload();
        return;
    }

    memcpy((uint8_t *)downlink_payload_samples + payload_offset, event->data_ptr, data_len);
    downlink_payload_bytes += data_len;
    if (downlink_payload_bytes < downlink_payload_length) {
        return;
    }
    if (downlink_payload_bytes != downlink_payload_length) {
        protocol_violation(websocket);
        discard_tts_audio_pending();
        reset_downlink_payload();
        return;
    }
    if (assistant_accepts_downlink()) {
        if (satellite_audio_enqueue_downlink(downlink_payload_samples,
                                             downlink_payload_bytes / sizeof(int16_t),
                                             tts_audio_pending, tts_audio_sequence) &&
            assistant_state == SATELLITE_ASSISTANT_SPEAKING) {
            satellite_ui_post_playback_activity();
        }
        if (idle_pending) {
            schedule_idle_completion(IDLE_DRAIN_MS);
        }
    }
    clear_tts_audio_pending();
    reset_downlink_payload();
}

static bool validate_assistant_state(const cJSON *message)
{
    const char *state;
    const cJSON *turn_id = cJSON_GetObjectItemCaseSensitive(message, "turn_id");
    const bool was_speaking = assistant_state == SATELLITE_ASSISTANT_SPEAKING;
    bool replaces_follow_up_turn;
    bool replaces_barge_in_turn;
    bool starts_proactive_turn;

    if (!json_string(message, "state", &state)) {
        return false;
    }
    if (strcmp(state, "idle") == 0) {
        if (!cJSON_IsNull(turn_id) &&
            (!cJSON_IsString(turn_id) || turn_id->valuestring == NULL ||
              strcmp(turn_id->valuestring, active_turn_id) != 0)) {
            return false;
        }
        // The server can close a follow-up window while its new response is
        // still arriving. Keep accepting that downlink until it drains.
        idle_pending = true;
        uint32_t idle_delay_ms = IDLE_DRAIN_MS;
        if ((assistant_state == SATELLITE_ASSISTANT_THINKING ||
             assistant_state == SATELLITE_ASSISTANT_SPEAKING) &&
            !satellite_audio_playback_pending()) {
            idle_delay_ms = IDLE_RESPONSE_START_MS;
        }
        schedule_idle_completion(idle_delay_ms);
        return true;
    }
    if (!cJSON_IsString(turn_id) || turn_id->valuestring == NULL || turn_id->valuestring[0] == '\0' ||
        strnlen(turn_id->valuestring, TURN_ID_MAX_BYTES + 1) > TURN_ID_MAX_BYTES) {
        return false;
    }
    if (strcmp(state, "wake_detected") != 0 && strcmp(state, "listening") != 0 &&
        strcmp(state, "thinking") != 0 && strcmp(state, "speaking") != 0 &&
        strcmp(state, "follow_up") != 0 && strcmp(state, "error") != 0) {
        return false;
    }
    if (stop_pending) {
        return strcmp(turn_id->valuestring, active_turn_id) == 0;
    }
    // A spoken follow-up starts a new server turn before the previous
    // follow-up window expires, so it legitimately carries a new turn ID.
    replaces_follow_up_turn = assistant_state == SATELLITE_ASSISTANT_FOLLOW_UP &&
                               strcmp(active_turn_id, turn_id->valuestring) != 0;
    replaces_barge_in_turn = barge_in_turn_pending &&
                              (strcmp(state, "wake_detected") == 0 || strcmp(state, "listening") == 0) &&
                              strcmp(active_turn_id, turn_id->valuestring) != 0;
    // Server-initiated announcements start directly at speaking, without a
    // wake/listen/thinking sequence from this satellite.
    starts_proactive_turn = strcmp(state, "speaking") == 0 && active_turn_id[0] == '\0';
    if (strcmp(state, "wake_detected") == 0) {
        if (active_turn_id[0] != '\0' && !replaces_follow_up_turn && !replaces_barge_in_turn) {
            return false;
        }
    } else if ((!starts_proactive_turn && active_turn_id[0] == '\0') ||
               (strcmp(active_turn_id, turn_id->valuestring) != 0 && !replaces_follow_up_turn &&
                 !replaces_barge_in_turn && !starts_proactive_turn)) {
        return false;
    }
    if (replaces_follow_up_turn) {
        satellite_audio_cancel_playback();
    }
    idle_pending = false;
    if (idle_drain_timer != NULL && esp_timer_is_active(idle_drain_timer)) {
        esp_timer_stop(idle_drain_timer);
    }
    memcpy(active_turn_id, turn_id->valuestring, strlen(turn_id->valuestring) + 1);
    if (strcmp(state, "speaking") == 0 && !was_speaking) {
        expected_downlink_sequence = 0;
        last_played_sequence_valid = false;
        clear_tts_audio_pending();
    }
    barge_in_turn_pending = false;
    if (strcmp(state, "wake_detected") == 0) {
        assistant_state = SATELLITE_ASSISTANT_WAKE_DETECTED;
    } else if (strcmp(state, "listening") == 0) {
        assistant_state = SATELLITE_ASSISTANT_LISTENING;
    } else if (strcmp(state, "thinking") == 0) {
        assistant_state = SATELLITE_ASSISTANT_THINKING;
    } else if (strcmp(state, "speaking") == 0) {
        assistant_state = SATELLITE_ASSISTANT_SPEAKING;
    } else if (strcmp(state, "follow_up") == 0) {
        assistant_state = SATELLITE_ASSISTANT_FOLLOW_UP;
        const cJSON *expires_at_ms = cJSON_GetObjectItemCaseSensitive(message, "expires_at_ms");
        if (cJSON_IsNumber(expires_at_ms) && expires_at_ms->valuedouble > 0) {
            const int64_t remaining_ms = (int64_t)expires_at_ms->valuedouble - (int64_t)time(NULL) * 1000;
            satellite_ui_post_follow_up_expiry(remaining_ms > 0 && remaining_ms < UINT32_MAX
                                                   ? (uint32_t)remaining_ms
                                                   : 0);
        }
    } else {
        assistant_state = SATELLITE_ASSISTANT_ERROR;
    }
    if (assistant_state == SATELLITE_ASSISTANT_THINKING) {
        // The server has finalized VAD for this turn. Do not send locally
        // buffered tail audio into the next server transcription window.
        satellite_audio_discard_uplink();
    }
    if (assistant_state == SATELLITE_ASSISTANT_SPEAKING && !was_speaking) {
        satellite_audio_begin_playback();
    } else if (was_speaking) {
        satellite_audio_log_playback_diagnostics();
    }
    return true;
}

static bool validate_tts_cancel(const cJSON *message)
{
    const char *turn_id;
    return json_string(message, "turn_id", &turn_id) && active_turn_id[0] != '\0' &&
           strcmp(active_turn_id, turn_id) == 0 && assistant_state == SATELLITE_ASSISTANT_SPEAKING;
}

static bool validate_tts_end(const cJSON *message)
{
    return validate_tts_cancel(message);
}

static bool handle_control_message(esp_websocket_client_handle_t websocket, const char *data, size_t length)
{
    cJSON *message = cJSON_ParseWithLength(data, length);
    const char *type;
    bool valid = false;

    if (message != NULL && cJSON_IsObject(message) && json_string(message, "type", &type)) {
        ESP_LOGI(TAG, "Received server control message: %s", type);
        if (!protocol_ready) {
            valid = strcmp(type, "satellite.welcome") == 0 && validate_welcome(message);
        } else if (tts_audio_pending) {
            // tts.audio is a one-message header for the binary frame that follows.
            discard_tts_audio_pending();
            valid = false;
        } else if (strcmp(type, "tts.audio") == 0) {
            valid = validate_tts_audio(message);
        } else if (strcmp(type, "assistant.state") == 0) {
            clear_tts_audio_pending();
            valid = validate_assistant_state(message);
            if (valid) {
                satellite_ui_post_assistant_state(assistant_state);
            }
        } else if (strcmp(type, "tts.cancel") == 0) {
            valid = validate_tts_cancel(message);
            if (valid) {
                barge_in_turn_pending = true;
                satellite_audio_cancel_playback();
            }
        } else if (strcmp(type, "tts.end") == 0) {
            valid = validate_tts_end(message);
            if (valid) {
                // Keep the speaker active until the queued I2S PCM drains.
                idle_pending = true;
                schedule_idle_completion(IDLE_DRAIN_MS);
            }
        } else if (strcmp(type, "satellite.health_request") == 0) {
            const char *request_id;
            valid = json_string(message, "id", &request_id) && request_id[0] != '\0' &&
                    respond_to_health_request(request_id);
        } else if (strcmp(type, "conversation_mode.changed") == 0 ||
                   strcmp(type, "conversation.transcript") == 0 ||
                   strcmp(type, "conversation.response") == 0) {
            valid = true;
        }
    }
    cJSON_Delete(message);
    if (!valid) {
        ESP_LOGW(TAG, "Server control message did not match the satellite contract");
        protocol_violation(websocket);
    }
    return valid;
}

esp_err_t satellite_protocol_init(void)
{
    esp_err_t error = read_nvs_string("device", "id", device_id, sizeof(device_id));
    if (error == ESP_OK) {
        error = read_nvs_string("device", "name", device_name, sizeof(device_name));
    }
    if (error == ESP_OK) {
        error = read_nvs_string("fulloch", "device_token", device_token, sizeof(device_token));
    }
    if (error != ESP_OK) {
        return error;
    }
    if (downlink_payload_samples == NULL) {
        downlink_payload_samples = heap_caps_malloc(DOWNLINK_MAX_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (downlink_payload_samples == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    websocket_send_lock = xSemaphoreCreateMutex();
    if (websocket_send_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const esp_timer_create_args_t idle_timer_config = {
        .callback = idle_drain_timer_callback,
        .name = "idle_drain",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&idle_timer_config, &idle_drain_timer), TAG,
                        "Idle drain timer creation failed");
    const esp_timer_create_args_t recovery_timer_config = {
        .callback = recovery_timer_callback,
        .name = "tts_recovery",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&recovery_timer_config, &recovery_timer), TAG,
                        "TTS recovery timer creation failed");
    return ESP_OK;
}

void satellite_protocol_reset(void)
{
    // A blocked transport send may own this lock indefinitely. Invalidate the
    // session before stopping the client so recovery never waits on that task.
    if (protocol_ready) {
        ESP_LOGI(TAG, "Protocol session ended after sending %lu uplink audio frames",
                 (unsigned long)uplink_frames_sent);
    }
    protocol_ready = false;
    uplink_frames_sent = 0;
    last_uplink_send_us = 0;
    reset_downlink_payload();
    protocol_websocket = NULL;
    clear_recovery_state();
    stop_pending = false;
    idle_pending = false;
    barge_in_turn_pending = false;
    assistant_state = SATELLITE_ASSISTANT_IDLE;
    satellite_ui_post_assistant_state(assistant_state);
    if (idle_drain_timer != NULL && esp_timer_is_active(idle_drain_timer)) {
        esp_timer_stop(idle_drain_timer);
    }
}

void satellite_protocol_disconnected(void)
{
    const bool can_recover = protocol_ready && session_id[0] != '\0' && active_turn_id[0] != '\0' &&
                             assistant_state == SATELLITE_ASSISTANT_SPEAKING;
    protocol_ready = false;
    protocol_websocket = NULL;
    reset_downlink_payload();
    clear_tts_audio_pending();
    if (!can_recover) {
        satellite_protocol_reset();
        satellite_audio_reset();
        return;
    }
    recovery_pending = true;
    // Queued PCM may overlap the server's resumed stream, so discard it before reconnecting.
    satellite_audio_cancel_playback();
    ESP_LOGW(TAG, "Preserving TTS turn %s through a %u ms reconnect window", active_turn_id,
             TTS_RECOVERY_WINDOW_MS);
    if (esp_timer_start_once(recovery_timer, TTS_RECOVERY_WINDOW_MS * 1000ULL) != ESP_OK) {
        recovery_timer_callback(NULL);
    }
}

bool satellite_protocol_recovery_pending(void)
{
    return recovery_pending;
}

bool satellite_protocol_connected(esp_websocket_client_handle_t websocket)
{
    cJSON *hello = cJSON_CreateObject();
    if (hello == NULL) {
        return false;
    }
    cJSON *version = cJSON_AddObjectToObject(hello, "protocol");
    cJSON *device = cJSON_AddObjectToObject(hello, "device");
    cJSON *capabilities = cJSON_AddObjectToObject(hello, "capabilities");
    char *serialized;

    if (hello == NULL || version == NULL || device == NULL || capabilities == NULL) {
        cJSON_Delete(hello);
        return false;
    }
    const esp_app_desc_t *app = esp_app_get_description();
    cJSON_AddStringToObject(hello, "type", "satellite.hello");
    cJSON_AddStringToObject(hello, "token", device_token);
    cJSON_AddStringToObject(version, "name", "satellite-v2");
    cJSON_AddNumberToObject(version, "major", 2);
    cJSON_AddNumberToObject(version, "minor", 3);
    cJSON_AddStringToObject(device, "id", device_id);
    cJSON_AddStringToObject(device, "name", device_name);
    cJSON_AddStringToObject(device, "firmware", "0.1.0");
    cJSON_AddStringToObject(device, "build", app->version);
    cJSON_AddStringToObject(device, "board", FULLOCH_BOARD);
    cJSON_AddBoolToObject(capabilities, "audio_input", true);
    cJSON_AddBoolToObject(capabilities, "audio_output", true);
    cJSON_AddBoolToObject(capabilities, "display", true);
    cJSON_AddBoolToObject(capabilities, "touch", true);
    cJSON_AddStringToObject(capabilities, "presence_indicator", "display_glow");
    cJSON_AddBoolToObject(capabilities, "conversation_ui", true);
    cJSON_AddBoolToObject(capabilities, "ota", false);
    if (recovery_pending) {
        cJSON *resume = cJSON_AddObjectToObject(hello, "resume");
        if (resume == NULL) {
            cJSON_Delete(hello);
            return false;
        }
        cJSON_AddStringToObject(resume, "session_id", session_id);
        cJSON_AddStringToObject(resume, "turn_id", active_turn_id);
        cJSON_AddNumberToObject(resume, "next_seq", last_played_sequence_valid ? last_played_sequence + 1 : 0);
    }
    serialized = cJSON_PrintUnformatted(hello);
    cJSON_Delete(hello);
    if (serialized == NULL || strlen(serialized) > CONTROL_MESSAGE_MAX_BYTES) {
        cJSON_free(serialized);
        return false;
    }
    if (xSemaphoreTake(websocket_send_lock, pdMS_TO_TICKS(1000)) != pdPASS) {
        cJSON_free(serialized);
        return false;
    }
    const bool sent = esp_websocket_client_send_text(websocket, serialized, strlen(serialized),
                                                      pdMS_TO_TICKS(1000)) >= 0;
    xSemaphoreGive(websocket_send_lock);
    cJSON_free(serialized);
    if (!sent) {
        ESP_LOGW(TAG, "Unable to send satellite handshake");
    } else {
        protocol_websocket = websocket;
    }
    return sent;
}

void satellite_protocol_handle_data(esp_websocket_client_handle_t websocket,
                                    const esp_websocket_event_data_t *event)
{
    if (event != NULL && (event->op_code == WEBSOCKET_OPCODE_CLOSE || event->op_code == WEBSOCKET_OPCODE_PING ||
                           event->op_code == WEBSOCKET_OPCODE_PONG)) {
        return;
    }
    if (event == NULL) {
        protocol_violation(websocket);
        return;
    }
    if (event->op_code == WEBSOCKET_OPCODE_TEXT) {
        if (event->payload_offset != 0 || !event->fin || event->data_len != event->payload_len ||
            event->data_len <= 0 || event->data_len > CONTROL_MESSAGE_MAX_BYTES) {
            protocol_violation(websocket);
            return;
        }
        handle_control_message(websocket, event->data_ptr, event->data_len);
    } else if (event->op_code == WEBSOCKET_OPCODE_BINARY) {
        handle_binary_message(websocket, event);
    } else {
        protocol_violation(websocket);
    }
}

esp_err_t satellite_protocol_send_uplink(const int16_t *samples, size_t sample_count)
{
    if (!protocol_ready || samples == NULL || sample_count != 320) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(websocket_send_lock, pdMS_TO_TICKS(1000)) != pdPASS) {
        return ESP_ERR_TIMEOUT;
    }
    if (!protocol_ready || protocol_websocket == NULL) {
        xSemaphoreGive(websocket_send_lock);
        return ESP_ERR_INVALID_STATE;
    }
    const int sent_bytes = esp_websocket_client_send_bin(protocol_websocket, (const char *)samples, 640,
                                                          pdMS_TO_TICKS(WEBSOCKET_SEND_TIMEOUT_MS));
    xSemaphoreGive(websocket_send_lock);
    if (sent_bytes != 640) {
        reconnect_after_send_failure();
        return ESP_FAIL;
    }
    const int64_t now_us = esp_timer_get_time();
    if (uplink_frames_sent < 5) {
        ESP_LOGI(TAG, "Sent uplink frame %lu: %d bytes, interval=%lld us", (unsigned long)(uplink_frames_sent + 1),
                 sent_bytes, (long long)(last_uplink_send_us == 0 ? 0 : now_us - last_uplink_send_us));
    }
    last_uplink_send_us = now_us;
    uplink_frames_sent++;
    return ESP_OK;
}

satellite_assistant_state_t satellite_protocol_assistant_state(void)
{
    return assistant_state;
}

bool satellite_protocol_ready(void)
{
    return protocol_ready;
}

esp_err_t satellite_protocol_send_stop(void)
{
    static const char stop[] = "{\"type\":\"satellite.stop\"}";
    if (!protocol_ready || protocol_websocket == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(websocket_send_lock, pdMS_TO_TICKS(1000)) != pdPASS) {
        return ESP_ERR_TIMEOUT;
    }
    const bool sent = esp_websocket_client_send_text(protocol_websocket, stop, sizeof(stop) - 1,
                                                      pdMS_TO_TICKS(1000)) >= 0;
    xSemaphoreGive(websocket_send_lock);

    // Stop locally without waiting for the server's idle acknowledgement.
    stop_pending = sent;
    idle_pending = false;
    if (idle_drain_timer != NULL && esp_timer_is_active(idle_drain_timer)) {
        esp_timer_stop(idle_drain_timer);
    }
    reset_downlink_payload();
    clear_recovery_state();
    satellite_audio_reset();
    assistant_state = SATELLITE_ASSISTANT_IDLE;
    satellite_ui_post_assistant_state(assistant_state);
    return sent ? ESP_OK : ESP_FAIL;
}

void satellite_protocol_playback_frame_completed(uint32_t sequence)
{
    if (protocol_ready || recovery_pending) {
        last_played_sequence = sequence;
        last_played_sequence_valid = true;
    }
}
