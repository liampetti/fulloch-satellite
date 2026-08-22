#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "jpeg_decoder.h"
#include "lvgl.h"
#include "nvs.h"
#include "sonos.h"
#include "ui.h"

#define SONOS_HOST_SIZE 64
#define SONOS_URL_SIZE 128
#define SONOS_RESPONSE_SIZE (256 * 1024)
#define SONOS_ARTWORK_SIZE (128 * 128 * sizeof(lv_color_t))
#define SONOS_QUEUE_DEPTH 4
#define SONOS_TASK_STACK_BYTES 12288

static const char *TAG = "sonos";
static QueueHandle_t requests;
static char host[SONOS_HOST_SIZE];
static bool playing;
static uint8_t *artwork_data;
static lv_img_dsc_t artwork;

typedef struct {
    char body[SONOS_RESPONSE_SIZE];
    size_t length;
} sonos_response_t;

static sonos_response_t *response;

static esp_err_t response_handler(esp_http_client_event_t *event)
{
    sonos_response_t *response = event->user_data;
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

static int request(const char *service, const char *action, const char *body)
{
    char url[SONOS_URL_SIZE];
    snprintf(url, sizeof(url), "http://%s:1400/MediaRenderer/%s/Control", host, service);
    *response = (sonos_response_t){0};
    const esp_http_client_config_t config = {
        .url = url,
        .event_handler = response_handler,
        .user_data = response,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return -1;
    }
    char soap_action[160];
    snprintf(soap_action, sizeof(soap_action), "\"urn:schemas-upnp-org:service:%s:1#%s\"", service, action);
    esp_http_client_set_method(client, HTTP_METHOD_POST);
    esp_http_client_set_header(client, "Content-Type", "text/xml; charset=\"utf-8\"");
    esp_http_client_set_header(client, "SOAPACTION", soap_action);
    esp_http_client_set_post_field(client, body, strlen(body));
    const esp_err_t error = esp_http_client_perform(client);
    const int status = error == ESP_OK ? esp_http_client_get_status_code(client) : -1;
    if (error != ESP_OK) {
        ESP_LOGW(TAG, "Sonos request failed (%s, errno=%d)", esp_err_to_name(error), esp_http_client_get_errno(client));
    }
    esp_http_client_cleanup(client);
    return status;
}

static int download_artwork(const char *url)
{
    *response = (sonos_response_t){0};
    const esp_http_client_config_t config = {
        .url = url,
        .event_handler = response_handler,
        .user_data = response,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return -1;
    }
    const esp_err_t error = esp_http_client_perform(client);
    const int status = error == ESP_OK ? esp_http_client_get_status_code(client) : -1;
    esp_http_client_cleanup(client);
    return status;
}

static const char *soap_body(const char *service, const char *action)
{
    static char body[384];
    const char *play_speed = strcmp(action, "Play") == 0 ? "<Speed>1</Speed>" : "";
    snprintf(body, sizeof(body),
             "<?xml version=\"1.0\"?><s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\">"
             "<s:Body><u:%s xmlns:u=\"urn:schemas-upnp-org:service:%s:1\">"
             "<InstanceID>0</InstanceID>%s</u:%s></s:Body></s:Envelope>", action, service, play_speed, action);
    return body;
}

static bool xml_value(const char *xml, const char *name, char *value, size_t value_size)
{
    char start_tag[64];
    char end_tag[64];
    snprintf(start_tag, sizeof(start_tag), "<%s>", name);
    snprintf(end_tag, sizeof(end_tag), "</%s>", name);
    const char *start = strstr(xml, start_tag);
    if (start == NULL) {
        return false;
    }
    start += strlen(start_tag);
    const char *end = strstr(start, end_tag);
    if (end == NULL || end == start || (size_t)(end - start) >= value_size) {
        return false;
    }
    memcpy(value, start, end - start);
    value[end - start] = '\0';
    return true;
}

static size_t encode_utf8(char *output, uint32_t codepoint)
{
    if (codepoint <= 0x7f) {
        output[0] = (char)codepoint;
        return 1;
    }
    if (codepoint <= 0x7ff) {
        output[0] = (char)(0xc0 | (codepoint >> 6));
        output[1] = (char)(0x80 | (codepoint & 0x3f));
        return 2;
    }
    if (codepoint <= 0xffff) {
        output[0] = (char)(0xe0 | (codepoint >> 12));
        output[1] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
        output[2] = (char)(0x80 | (codepoint & 0x3f));
        return 3;
    }
    output[0] = (char)(0xf0 | (codepoint >> 18));
    output[1] = (char)(0x80 | ((codepoint >> 12) & 0x3f));
    output[2] = (char)(0x80 | ((codepoint >> 6) & 0x3f));
    output[3] = (char)(0x80 | (codepoint & 0x3f));
    return 4;
}

static bool decode_numeric_entity(const char *text, char *output, size_t *consumed, size_t *output_size)
{
    if (text[0] != '&' || text[1] != '#') {
        return false;
    }
    size_t index = 2;
    const int base = text[index] == 'x' || text[index] == 'X' ? 16 : 10;
    index += base == 16;
    uint32_t codepoint = 0;
    const size_t digits_start = index;
    for (; text[index] != '\0' && text[index] != ';'; ++index) {
        const char character = text[index];
        const int digit = character >= '0' && character <= '9' ? character - '0' :
                          character >= 'a' && character <= 'f' ? character - 'a' + 10 :
                          character >= 'A' && character <= 'F' ? character - 'A' + 10 : -1;
        if (digit < 0 || digit >= base || codepoint > (0x10ffff - (uint32_t)digit) / (uint32_t)base) {
            return false;
        }
        codepoint = codepoint * base + digit;
    }
    if (index == digits_start || text[index] != ';' || codepoint == 0 || codepoint > 0x10ffff ||
        (codepoint >= 0xd800 && codepoint <= 0xdfff)) {
        return false;
    }
    *output_size = encode_utf8(output, codepoint);
    *consumed = index + 1;
    return true;
}

static void decode_entities(char *text)
{
    char *output = text;
    while (*text != '\0') {
        size_t consumed;
        size_t output_size;
        if (strncmp(text, "&amp;", 5) == 0) {
            *output++ = '&'; text += 5;
        } else if (strncmp(text, "&lt;", 4) == 0) {
            *output++ = '<'; text += 4;
        } else if (strncmp(text, "&gt;", 4) == 0) {
            *output++ = '>'; text += 4;
        } else if (strncmp(text, "&quot;", 6) == 0) {
            *output++ = '"'; text += 6;
        } else if (strncmp(text, "&apos;", 6) == 0) {
            *output++ = '\''; text += 6;
        } else if (decode_numeric_entity(text, output, &consumed, &output_size)) {
            output += output_size;
            text += consumed;
        } else {
            *output++ = *text++;
        }
    }
    *output = '\0';
}

static void update_playback_state(void)
{
    if (request("AVTransport", "GetPositionInfo", soap_body("AVTransport", "GetPositionInfo")) != 200) {
        satellite_ui_post_music_message("Sonos unavailable");
        return;
    }
    char metadata[4096] = {0};
    char title[96] = {0};
    char artist[96] = {0};
    char artwork_uri[256] = {0};
    if (xml_value(response->body, "TrackMetaData", metadata, sizeof(metadata))) {
        decode_entities(metadata);
        // DIDL metadata is itself entity-escaped inside the SOAP response.
        decode_entities(metadata);
        xml_value(metadata, "dc:title", title, sizeof(title));
        if (!xml_value(metadata, "dc:creator", artist, sizeof(artist))) {
            xml_value(metadata, "upnp:artist", artist, sizeof(artist));
        }
        xml_value(metadata, "upnp:albumArtURI", artwork_uri, sizeof(artwork_uri));
    }
    if (request("AVTransport", "GetTransportInfo", soap_body("AVTransport", "GetTransportInfo")) != 200) {
        satellite_ui_post_music_message("Sonos unavailable");
        return;
    }
    char state[32] = {0};
    xml_value(response->body, "CurrentTransportState", state, sizeof(state));
    playing = strcmp(state, "PLAYING") == 0;
    satellite_ui_post_music_state(title[0] == '\0' ? "Nothing playing" : title, playing);
    satellite_ui_post_music_message(title[0] == '\0' ? "Sonos" : artist);
    if (artwork_uri[0] == '\0') {
        return;
    }
    char artwork_url[384];
    if (strncmp(artwork_uri, "http://", 7) == 0 || strncmp(artwork_uri, "https://", 8) == 0) {
        strlcpy(artwork_url, artwork_uri, sizeof(artwork_url));
    } else {
        snprintf(artwork_url, sizeof(artwork_url), "http://%s:1400%s", host, artwork_uri);
    }
    const int artwork_status = download_artwork(artwork_url);
    if (artwork_status != 200) {
        ESP_LOGW(TAG, "Sonos album art download failed (HTTP %d)", artwork_status);
        return;
    }
    esp_jpeg_image_cfg_t config = {
        .indata = (uint8_t *)response->body,
        .indata_size = response->length,
        .outbuf = artwork_data,
        .outbuf_size = SONOS_ARTWORK_SIZE,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .flags = {.swap_color_bytes = 1},
    };
    esp_jpeg_image_output_t output;
    bool decoded = false;
    const esp_jpeg_image_scale_t scales[] = {
        JPEG_IMAGE_SCALE_0,
        JPEG_IMAGE_SCALE_1_2,
        JPEG_IMAGE_SCALE_1_4,
        JPEG_IMAGE_SCALE_1_8,
    };
    for (size_t index = 0; index < sizeof(scales) / sizeof(scales[0]); ++index) {
        config.out_scale = scales[index];
        if (esp_jpeg_get_image_info(&config, &output) != ESP_OK || output.output_len > SONOS_ARTWORK_SIZE) {
            continue;
        }
        decoded = esp_jpeg_decode(&config, &output) == ESP_OK;
        if (decoded) {
            break;
        }
    }
    if (!decoded) {
        ESP_LOGW(TAG, "Sonos album art decode failed");
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

static void perform_command(satellite_sonos_command_t command)
{
    if (command == SATELLITE_SONOS_REFRESH) {
        update_playback_state();
        return;
    }
    const char *action = command == SATELLITE_SONOS_PREVIOUS ? "Previous" :
                         command == SATELLITE_SONOS_NEXT ? "Next" : playing ? "Pause" : "Play";
    if (request("AVTransport", action, soap_body("AVTransport", action)) != 200) {
        satellite_ui_post_music_message("Sonos control failed");
        return;
    }
    update_playback_state();
}

static void sonos_task(void *argument)
{
    (void)argument;
    satellite_sonos_command_t command;
    while (xQueueReceive(requests, &command, portMAX_DELAY) == pdPASS) {
        perform_command(command);
    }
}

static bool start(void)
{
    if (requests != NULL) {
        return true;
    }
    nvs_handle_t handle;
    size_t host_size = sizeof(host);
    if (nvs_open("sonos", NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    if (nvs_get_str(handle, "host", host, &host_size) != ESP_OK) {
        nvs_close(handle);
        return false;
    }
    nvs_close(handle);
    response = heap_caps_calloc(1, sizeof(*response), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    artwork_data = heap_caps_malloc(SONOS_ARTWORK_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    requests = xQueueCreate(SONOS_QUEUE_DEPTH, sizeof(satellite_sonos_command_t));
    if (response == NULL || artwork_data == NULL || requests == NULL ||
        xTaskCreateWithCaps(sonos_task, "sonos", SONOS_TASK_STACK_BYTES, NULL, 4, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        free(response);
        free(artwork_data);
        response = NULL;
        artwork_data = NULL;
        if (requests != NULL) {
            vQueueDelete(requests);
        }
        requests = NULL;
        return false;
    }
    ESP_LOGI(TAG, "Sonos controls are ready");
    return true;
}

const lv_img_dsc_t *satellite_sonos_album_art(void)
{
    return artwork.data == NULL ? NULL : &artwork;
}

void satellite_sonos_request(satellite_sonos_command_t command)
{
    if (!start()) {
        satellite_ui_post_music_message("Sonos not configured");
        return;
    }
    if (xQueueSend(requests, &command, 0) != pdPASS) {
        ESP_LOGW(TAG, "Sonos request queue is full");
    }
}
