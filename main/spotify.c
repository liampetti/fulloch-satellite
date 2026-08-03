#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "connectivity.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_tls.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "jpeg_decoder.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "spotify.h"
#include "ui.h"

#define SPOTIFY_QUEUE_DEPTH 4
#define SPOTIFY_ACCESS_TOKEN_SIZE 1024
#define SPOTIFY_CLIENT_ID_SIZE 257
#define SPOTIFY_CLIENT_SECRET_SIZE 257
#define SPOTIFY_REFRESH_TOKEN_SIZE 513
#define SPOTIFY_DEVICE_ID_SIZE 257
#define SPOTIFY_RESPONSE_SIZE 16384
#define SPOTIFY_ARTWORK_SIZE (128 * 128 * sizeof(lv_color_t))
#define SPOTIFY_TASK_STACK_BYTES 12288
#define SPOTIFY_BASIC_AUTH_SIZE 704

static const char *TAG = "spotify";
static QueueHandle_t requests;
static char client_id[SPOTIFY_CLIENT_ID_SIZE];
static char client_secret[SPOTIFY_CLIENT_SECRET_SIZE];
static char refresh_token[SPOTIFY_REFRESH_TOKEN_SIZE];
static char playback_device_id[SPOTIFY_DEVICE_ID_SIZE];
static char access_token[SPOTIFY_ACCESS_TOKEN_SIZE];
static char last_track[96];
static char encoded_refresh_token[SPOTIFY_REFRESH_TOKEN_SIZE * 3];
static char token_request_body[SPOTIFY_REFRESH_TOKEN_SIZE * 3 + 64];
static bool playing;
static uint8_t *artwork_data;
static lv_img_dsc_t artwork;

typedef struct {
    char body[SPOTIFY_RESPONSE_SIZE];
    size_t length;
    int tls_error;
    int tls_flags;
} spotify_response_t;

static spotify_response_t *response;

static void log_memory(const char *stage)
{
    ESP_LOGI(TAG, "%s memory: internal=%u/%u psram=%u/%u", stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

static esp_err_t read_nvs_string(nvs_handle_t handle, const char *key, char *value, size_t value_size)
{
    size_t required_size = value_size;
    return nvs_get_str(handle, key, value, &required_size);
}

static esp_err_t response_handler(esp_http_client_event_t *event)
{
    spotify_response_t *response = event->user_data;
    if (event->event_id == HTTP_EVENT_ERROR && response != NULL && event->data != NULL) {
        esp_tls_get_and_clear_last_error((esp_tls_error_handle_t)event->data, &response->tls_error,
                                         &response->tls_flags);
        return ESP_OK;
    }
    if (event->event_id != HTTP_EVENT_ON_DATA || response == NULL || event->data_len <= 0) {
        return ESP_OK;
    }
    const size_t available = sizeof(response->body) - response->length - 1;
    if ((size_t)event->data_len > available) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(response->body + response->length, event->data, event->data_len);
    response->length += event->data_len;
    response->body[response->length] = '\0';
    return ESP_OK;
}

static int request(const char *url, esp_http_client_method_t method, const char *body, bool authorized,
                   const char *authorization, spotify_response_t *response)
{
    *response = (spotify_response_t){0};
    const esp_http_client_config_t config = {
        .url = url,
        .event_handler = response_handler,
        .user_data = response,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return -1;
    }
    esp_http_client_set_method(client, method);
    if (authorized) {
        char authorization[SPOTIFY_ACCESS_TOKEN_SIZE + 8];
        snprintf(authorization, sizeof(authorization), "Bearer %s", access_token);
        esp_http_client_set_header(client, "Authorization", authorization);
    } else if (authorization != NULL) {
        esp_http_client_set_header(client, "Authorization", authorization);
    }
    if (body != NULL) {
        esp_http_client_set_header(client, "Content-Type", "application/x-www-form-urlencoded");
        esp_http_client_set_post_field(client, body, strlen(body));
    }
    const esp_err_t error = esp_http_client_perform(client);
    const int status = error == ESP_OK ? esp_http_client_get_status_code(client) : -1;
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Spotify HTTP request failed (%s, errno=%d, tls=%d, flags=0x%x)",
                 esp_err_to_name(error), esp_http_client_get_errno(client), response->tls_error,
                 response->tls_flags);
    }
    esp_http_client_cleanup(client);
    return status;
}

static bool client_authorization(char *authorization, size_t authorization_size)
{
    char credentials[SPOTIFY_CLIENT_ID_SIZE + SPOTIFY_CLIENT_SECRET_SIZE + 2];
    const int credentials_length = snprintf(credentials, sizeof(credentials), "%s:%s", client_id, client_secret);
    size_t encoded_length;
    if (credentials_length < 0 || credentials_length >= (int)sizeof(credentials) || authorization_size < 7 ||
        mbedtls_base64_encode((unsigned char *)authorization + 6, authorization_size - 6, &encoded_length,
                              (const unsigned char *)credentials, credentials_length) != 0) {
        return false;
    }
    memcpy(authorization, "Basic ", 6);
    authorization[encoded_length + 6] = '\0';
    return true;
}

static bool url_encode(char *output, size_t output_size, const char *input)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t output_length = 0;
    for (; *input != '\0'; ++input) {
        const unsigned char character = (unsigned char)*input;
        const bool unreserved = (character >= 'a' && character <= 'z') ||
                                (character >= 'A' && character <= 'Z') ||
                                (character >= '0' && character <= '9') || character == '-' ||
                                character == '_' || character == '.' || character == '~';
        const size_t required = unreserved ? 1 : 3;
        if (output_length + required >= output_size) {
            return false;
        }
        if (unreserved) {
            output[output_length++] = character;
        } else {
            output[output_length++] = '%';
            output[output_length++] = hex[character >> 4];
            output[output_length++] = hex[character & 0x0f];
        }
    }
    output[output_length] = '\0';
    return true;
}

static bool refresh_access_token(void)
{
    char authorization[SPOTIFY_BASIC_AUTH_SIZE];
    if (!url_encode(encoded_refresh_token, sizeof(encoded_refresh_token), refresh_token) ||
        !client_authorization(authorization, sizeof(authorization))) {
        ESP_LOGW(TAG, "Spotify credentials cannot be encoded");
        return false;
    }
    snprintf(token_request_body, sizeof(token_request_body), "grant_type=refresh_token&refresh_token=%s",
             encoded_refresh_token);
    const int status = request("https://accounts.spotify.com/api/token", HTTP_METHOD_POST, token_request_body,
                               false, authorization, response);
    if (status != 200) {
        ESP_LOGW(TAG, "Spotify token refresh failed (HTTP %d)", status);
        satellite_ui_post_music_message(status == 401 ? "Spotify client rejected" :
                                        status == 400 ? "Spotify token rejected" :
                                        status < 0 && response->tls_error != 0 ? "Spotify TLS error" :
                                        status < 0 ? "Spotify network error" : "Spotify sign-in failed");
        return false;
    }
    cJSON *json = cJSON_Parse(response->body);
    const cJSON *token = json == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(json, "access_token");
    const bool valid = cJSON_IsString(token) && token->valuestring != NULL &&
                       strlcpy(access_token, token->valuestring, sizeof(access_token)) < sizeof(access_token);
    cJSON_Delete(json);
    if (!valid) {
        ESP_LOGW(TAG, "Spotify token response was invalid");
        satellite_ui_post_music_message("Spotify sign-in failed");
    }
    return valid;
}

static bool resolve_playback_device(void)
{
    playback_device_id[0] = '\0';
    if (request("https://api.spotify.com/v1/me/player/devices", HTTP_METHOD_GET, NULL, true, NULL, response) != 200) {
        ESP_LOGW(TAG, "Spotify device lookup failed");
        satellite_ui_post_music_message("Spotify device unavailable");
        return false;
    }
    cJSON *json = cJSON_Parse(response->body);
    const cJSON *devices = json == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(json, "devices");
    cJSON *device = NULL;
    cJSON_ArrayForEach(device, devices) {
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(device, "id");
        const cJSON *is_active = cJSON_GetObjectItemCaseSensitive(device, "is_active");
        if (!cJSON_IsString(id) || id->valuestring == NULL) {
            continue;
        }
        if (cJSON_IsTrue(is_active)) {
            strlcpy(playback_device_id, id->valuestring, sizeof(playback_device_id));
            break;
        }
    }
    cJSON_Delete(json);
    if (playback_device_id[0] == '\0') {
        ESP_LOGW(TAG, "Spotify playback device is unavailable");
        satellite_ui_post_music_message("No active Spotify device");
        return false;
    }
    return true;
}

static void update_playback_state(bool download_artwork)
{
    const int status = request("https://api.spotify.com/v1/me/player", HTTP_METHOD_GET, NULL, true, NULL, response);
    if (status == 204) {
        playing = false;
        satellite_ui_post_music_state(last_track[0] == '\0' ? "Nothing playing" : last_track, false);
        satellite_ui_post_music_message(last_track[0] == '\0' ? "Nothing playing" : "Last played");
        return;
    }
    if (status != 200) {
        ESP_LOGW(TAG, "Spotify playback status request failed");
        satellite_ui_post_music_message("Spotify playback unavailable");
        return;
    }
    cJSON *json = cJSON_Parse(response->body);
    const cJSON *is_playing = json == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(json, "is_playing");
    const cJSON *item = json == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(json, "item");
    const cJSON *name = item == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(item, "name");
    if (cJSON_IsBool(is_playing) && cJSON_IsString(name) && name->valuestring != NULL) {
        playing = cJSON_IsTrue(is_playing);
        strlcpy(last_track, name->valuestring, sizeof(last_track));
        satellite_ui_post_music_state(name->valuestring, playing);
        satellite_ui_post_music_message("");
    }
    if (!download_artwork) {
        cJSON_Delete(json);
        return;
    }
    const cJSON *album = item == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(item, "album");
    const cJSON *images = album == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(album, "images");
    const int image_index = cJSON_GetArraySize(images) - 1;
    const cJSON *image = image_index < 0 ? NULL : cJSON_GetArrayItem(images, image_index);
    const cJSON *url = image == NULL ? NULL : cJSON_GetObjectItemCaseSensitive(image, "url");
    char artwork_url[256] = {0};
    if (cJSON_IsString(url) && url->valuestring != NULL) {
        strlcpy(artwork_url, url->valuestring, sizeof(artwork_url));
    }
    cJSON_Delete(json);
    if (artwork_url[0] == '\0') {
        return;
    }
    if (request(artwork_url, HTTP_METHOD_GET, NULL, false, NULL, response) != 200) {
        ESP_LOGW(TAG, "Spotify album art download failed");
        return;
    }
    esp_jpeg_image_cfg_t config = {
        .indata = (uint8_t *)response->body,
        .indata_size = response->length,
        .outbuf = artwork_data,
        .outbuf_size = SPOTIFY_ARTWORK_SIZE,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
        .flags = {.swap_color_bytes = 1},
    };
    esp_jpeg_image_output_t output;
    if (esp_jpeg_decode(&config, &output) != ESP_OK ||
        output.width * output.height * sizeof(lv_color_t) > SPOTIFY_ARTWORK_SIZE) {
        ESP_LOGW(TAG, "Spotify album art decode failed");
        return;
    }
    artwork.header.always_zero = 0;
    artwork.header.w = output.width;
    artwork.header.h = output.height;
    artwork.header.cf = LV_IMG_CF_TRUE_COLOR;
    artwork.data_size = output.width * output.height * sizeof(lv_color_t);
    artwork.data = artwork_data;
    satellite_ui_post_album_art();
}

static void perform_command(satellite_spotify_command_t command)
{
    log_memory("Before Spotify request");
    if (!satellite_connectivity_time_synchronized()) {
        satellite_ui_post_music_message("Waiting for clock sync");
        log_memory("After Spotify request");
        return;
    }
    if (!refresh_access_token()) {
        log_memory("After Spotify request");
        return;
    }
    if (command == SATELLITE_SPOTIFY_REFRESH) {
        update_playback_state(true);
        log_memory("After Spotify request");
        return;
    }
    if (!resolve_playback_device()) {
        log_memory("After Spotify request");
        return;
    }
    const char *path = command == SATELLITE_SPOTIFY_PREVIOUS ? "previous" :
                       command == SATELLITE_SPOTIFY_NEXT ? "next" : playing ? "pause" : "play";
    const esp_http_client_method_t method = command == SATELLITE_SPOTIFY_TOGGLE_PLAYBACK ? HTTP_METHOD_PUT : HTTP_METHOD_POST;
    char url[384];
    snprintf(url, sizeof(url), "https://api.spotify.com/v1/me/player/%s?device_id=%s", path,
             playback_device_id);
    const int status = request(url, method, NULL, true, NULL, response);
    if (status != 200 && status != 204) {
        ESP_LOGW(TAG, "Spotify playback command failed");
        satellite_ui_post_music_message(status == 404 ? "Playback device unavailable" : "Spotify playback failed");
        log_memory("After Spotify request");
        return;
    }
    if (command == SATELLITE_SPOTIFY_TOGGLE_PLAYBACK) {
        playing = !playing;
    }
    update_playback_state(true);
    log_memory("After Spotify request");
}

static void spotify_task(void *argument)
{
    (void)argument;
    satellite_spotify_command_t command;
    while (xQueueReceive(requests, &command, portMAX_DELAY) == pdPASS) {
        perform_command(command);
    }
}

esp_err_t satellite_spotify_start(void)
{
    if (requests != NULL) {
        return ESP_OK;
    }
    nvs_handle_t handle;
    esp_err_t error = nvs_open("spotify", NVS_READONLY, &handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "Spotify is not provisioned");
        return ESP_OK;
    }
    if (error != ESP_OK) {
        return error;
    }
    error = read_nvs_string(handle, "client_id", client_id, sizeof(client_id));
    if (error == ESP_OK) {
        error = read_nvs_string(handle, "client_secret", client_secret, sizeof(client_secret));
    }
    if (error == ESP_OK) {
        error = read_nvs_string(handle, "refresh_token", refresh_token, sizeof(refresh_token));
    }
    nvs_close(handle);
    if (error != ESP_OK) {
        return error;
    }
    response = heap_caps_calloc(1, sizeof(*response), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    artwork_data = heap_caps_malloc(SPOTIFY_ARTWORK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (response == NULL || artwork_data == NULL) {
        free(response);
        free(artwork_data);
        response = NULL;
        artwork_data = NULL;
        return ESP_ERR_NO_MEM;
    }
    requests = xQueueCreate(SPOTIFY_QUEUE_DEPTH, sizeof(satellite_spotify_command_t));
    if (requests == NULL ||
        xTaskCreateWithCaps(spotify_task, "spotify", SPOTIFY_TASK_STACK_BYTES, NULL, 4, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Spotify controls are ready");
    return ESP_OK;
}

void satellite_spotify_request(satellite_spotify_command_t command)
{
    if (requests == NULL) {
        if (command == SATELLITE_SPOTIFY_REFRESH) {
            satellite_ui_post_music_message("Checking Spotify...");
        }
        const esp_err_t error = satellite_spotify_start();
        if (error != ESP_OK) {
            ESP_LOGW(TAG, "Spotify controls are unavailable (%s)", esp_err_to_name(error));
            satellite_ui_post_music_message("Spotify unavailable");
            return;
        }
    }
    if (requests == NULL) {
        satellite_ui_post_music_message("Spotify not configured");
        return;
    }
    if (xQueueSend(requests, &command, 0) != pdPASS) {
        ESP_LOGW(TAG, "Spotify request queue is full");
    }
}

const lv_img_dsc_t *satellite_spotify_album_art(void)
{
    return artwork.data == NULL ? NULL : &artwork;
}
