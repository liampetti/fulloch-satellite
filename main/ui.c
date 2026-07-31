#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "esp_check.h"
#include "connectivity.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fulloch_sat_logo.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "protocol.h"
#include "spotify.h"
#include "ui.h"
#include "waveshare_touch_lcd_185c.h"

#define UI_WIDTH 360
#define UI_HEIGHT 360
#define UI_DRAW_ROWS 10
#define UI_EVENT_QUEUE_DEPTH 8
#define UI_ASSISTANT_EVENT_QUEUE_DEPTH 1
#define UI_CONNECTION_EVENT_QUEUE_DEPTH 1
#define UI_CLOCK_DISPLAY_MS 10000

typedef enum {
    UI_EVENT_CONNECTION,
    UI_EVENT_ASSISTANT,
    UI_EVENT_MUSIC,
    UI_EVENT_MUSIC_MESSAGE,
    UI_EVENT_ALBUM_ART,
} ui_event_type_t;

typedef enum {
    UI_IDLE_VIEW_DATETIME,
    UI_IDLE_VIEW_MUSIC,
} ui_idle_view_t;

typedef struct {
    ui_event_type_t type;
    int value;
    char track[96];
} ui_event_t;

static const char *TAG = "ui";
static QueueHandle_t ui_events;
static QueueHandle_t assistant_events;
static QueueHandle_t connection_events;
static esp_timer_handle_t lvgl_tick_timer;
static lv_color_t draw_buffer[UI_WIDTH * UI_DRAW_ROWS];
static lv_disp_draw_buf_t display_draw_buffer;
static lv_disp_drv_t display_driver;
static lv_indev_drv_t touch_driver;
static lv_obj_t *rings[3];
static lv_obj_t *status_label;
static lv_obj_t *clock_logo;
static lv_obj_t *clock_time_label;
static lv_obj_t *clock_date_label;
static lv_obj_t *clock_connection_led;
static lv_obj_t *music_artwork;
static lv_obj_t *music_album_art;
static lv_obj_t *music_record;
static lv_obj_t *music_track_label;
static lv_obj_t *music_message_label;
static lv_obj_t *music_controls;
static lv_obj_t *music_play_icon;
static bool music_playing;
static uint32_t clock_visible_until;
static ui_idle_view_t idle_view = UI_IDLE_VIEW_DATETIME;
static bool connection_status_suppressed;
static satellite_connection_status_t connection_state = SATELLITE_CONNECTION_STARTING;
static satellite_assistant_state_t assistant_state = SATELLITE_ASSISTANT_IDLE;

static void lvgl_tick(void *argument)
{
    (void)argument;
    lv_tick_inc(5);
}

static void display_flush(lv_disp_drv_t *driver, const lv_area_t *area, lv_color_t *colors)
{
    if (waveshare_touch_lcd_185c_draw_rgb565(area->x1, area->y1, area->x2, area->y2,
                                              (const uint16_t *)colors) != ESP_OK) {
        ESP_LOGE(TAG, "Display flush failed");
    }
    lv_disp_flush_ready(driver);
}

static void touch_read(lv_indev_drv_t *driver, lv_indev_data_t *data)
{
    (void)driver;
    uint16_t x;
    uint16_t y;
    if (waveshare_touch_lcd_185c_read_touch(&x, &y)) {
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PR;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

static bool idle_view_visible(void)
{
    return !lv_obj_has_flag(clock_time_label, LV_OBJ_FLAG_HIDDEN) ||
           !lv_obj_has_flag(music_artwork, LV_OBJ_FLAG_HIDDEN);
}

static void set_datetime_visible(bool visible)
{
    if (visible) {
        lv_obj_clear_flag(clock_logo, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(clock_time_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(clock_date_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(clock_connection_led, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(clock_logo, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(clock_time_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(clock_date_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(clock_connection_led, LV_OBJ_FLAG_HIDDEN);
    }
}

static void set_music_visible(bool visible)
{
    if (visible) {
        lv_obj_clear_flag(music_artwork, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(music_track_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(music_message_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(music_controls, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(music_artwork, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(music_track_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(music_message_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(music_controls, LV_OBJ_FLAG_HIDDEN);
    }
}

static void handle_music_control(lv_event_t *event)
{
    satellite_spotify_request((satellite_spotify_command_t)(uintptr_t)lv_event_get_user_data(event));
}

static void create_music_control(lv_obj_t *parent, lv_coord_t x, lv_coord_t size, const char *icon,
                                 bool primary, satellite_spotify_command_t command)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_clear_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(button, size, size);
    lv_obj_set_pos(button, x, 0);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(button, primary ? lv_color_hex(0x34d399) : lv_color_hex(0x14251f), 0);
    lv_obj_set_style_bg_color(button, primary ? lv_color_hex(0x6ee7b7) : lv_color_hex(0x1e3a31),
                              LV_STATE_PRESSED);
    lv_obj_set_style_border_width(button, primary ? 0 : 2, 0);
    lv_obj_set_style_border_color(button, lv_color_hex(0x34d399), 0);
    lv_obj_set_style_shadow_width(button, primary ? 14 : 6, 0);
    lv_obj_set_style_shadow_color(button, lv_color_hex(0x20d99a), 0);
    lv_obj_set_style_shadow_opa(button, primary ? LV_OPA_50 : LV_OPA_20, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_add_event_cb(button, handle_music_control, LV_EVENT_CLICKED, (void *)(uintptr_t)command);

    lv_obj_t *icon_label = lv_label_create(button);
    lv_label_set_text(icon_label, icon);
    lv_obj_set_style_text_color(icon_label, primary ? lv_color_hex(0x052e23) : lv_color_hex(0x6ee7b7), 0);
    lv_obj_set_style_text_font(icon_label, &lv_font_montserrat_22, 0);
    lv_obj_center(icon_label);
    if (command == SATELLITE_SPOTIFY_TOGGLE_PLAYBACK) {
        music_play_icon = icon_label;
    }
}

static void animate_connection_led(void *object, int32_t shadow_width)
{
    lv_obj_t *led = object;
    lv_obj_set_style_shadow_width(led, shadow_width, 0);
    lv_obj_set_style_shadow_opa(led, shadow_width * 16, 0);
}

static void update_datetime(void)
{
    char time_text[6];
    char date_text[32];
    if (!satellite_connectivity_time_synchronized()) {
        lv_label_set_text(clock_time_label, "--:--");
        lv_label_set_text(clock_date_label, "Syncing clock");
        return;
    }
    time_t now = time(NULL);
    struct tm local_time;

    if (localtime_r(&now, &local_time) == NULL ||
        strftime(time_text, sizeof(time_text), "%H:%M", &local_time) == 0 ||
        strftime(date_text, sizeof(date_text), "%A, %B %d", &local_time) == 0) {
        snprintf(time_text, sizeof(time_text), "--:--");
        snprintf(date_text, sizeof(date_text), "Clock unavailable");
    }
    lv_label_set_text(clock_time_label, time_text);
    lv_label_set_text(clock_date_label, date_text);
}

static void show_idle_view(ui_idle_view_t view)
{
    idle_view = view;
    if (view == UI_IDLE_VIEW_DATETIME) {
        update_datetime();
        set_music_visible(false);
        set_datetime_visible(true);
    } else {
        set_datetime_visible(false);
        set_music_visible(true);
        satellite_spotify_request(SATELLITE_SPOTIFY_REFRESH);
    }
    clock_visible_until = lv_tick_get() + UI_CLOCK_DISPLAY_MS;
}

static void handle_tap(lv_event_t *event)
{
    (void)event;
    if (assistant_state == SATELLITE_ASSISTANT_IDLE) {
        if (!idle_view_visible()) {
            show_idle_view(UI_IDLE_VIEW_DATETIME);
        } else {
            clock_visible_until = lv_tick_get() + UI_CLOCK_DISPLAY_MS;
        }
    } else if (assistant_state != SATELLITE_ASSISTANT_ERROR) {
        if (satellite_protocol_send_stop() != ESP_OK) {
            ESP_LOGW(TAG, "Unable to send stop request");
        }
    }
}

static void handle_swipe(lv_event_t *event)
{
    (void)event;
    if (assistant_state == SATELLITE_ASSISTANT_IDLE && idle_view_visible()) {
        show_idle_view(idle_view == UI_IDLE_VIEW_DATETIME ? UI_IDLE_VIEW_MUSIC : UI_IDLE_VIEW_DATETIME);
    }
}

static void setup_lvgl(void)
{
    lv_init();
    lv_disp_draw_buf_init(&display_draw_buffer, draw_buffer, NULL, UI_WIDTH * UI_DRAW_ROWS);
    lv_disp_drv_init(&display_driver);
    display_driver.hor_res = UI_WIDTH;
    display_driver.ver_res = UI_HEIGHT;
    display_driver.draw_buf = &display_draw_buffer;
    display_driver.flush_cb = display_flush;
    lv_disp_drv_register(&display_driver);

    lv_indev_drv_init(&touch_driver);
    touch_driver.type = LV_INDEV_TYPE_POINTER;
    touch_driver.read_cb = touch_read;
    lv_indev_drv_register(&touch_driver);
    lv_timer_set_period(touch_driver.read_timer, 10);

    lv_obj_t *screen = lv_scr_act();
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(screen, handle_tap, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(screen, handle_swipe, LV_EVENT_GESTURE, NULL);
    for (size_t index = 0; index < 3; ++index) {
        rings[index] = lv_obj_create(screen);
        lv_obj_clear_flag(rings[index], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_radius(rings[index], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(rings[index], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(rings[index], 4 - index, 0);
        lv_obj_set_style_border_color(rings[index], lv_color_hex(0x20d99a), 0);
        lv_obj_add_event_cb(rings[index], handle_tap, LV_EVENT_PRESSED, NULL);
        lv_obj_add_flag(rings[index], LV_OBJ_FLAG_HIDDEN);
    }
    status_label = lv_label_create(screen);
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xff6b6b), 0);
    lv_obj_center(status_label);
    lv_obj_add_flag(status_label, LV_OBJ_FLAG_HIDDEN);
    clock_logo = lv_img_create(screen);
    lv_img_set_src(clock_logo, &fulloch_sat_logo);
    lv_obj_set_pos(clock_logo, (UI_WIDTH - fulloch_sat_logo.header.w) / 2, 20);
    clock_time_label = lv_label_create(screen);
    lv_obj_set_width(clock_time_label, UI_WIDTH);
    lv_obj_set_pos(clock_time_label, 0, 112);
    lv_obj_set_style_text_color(clock_time_label, lv_color_hex(0x6ee7b7), 0);
    lv_obj_set_style_text_align(clock_time_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(clock_time_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_letter_space(clock_time_label, 2, 0);
    clock_date_label = lv_label_create(screen);
    lv_obj_set_width(clock_date_label, UI_WIDTH);
    lv_obj_set_pos(clock_date_label, 0, 176);
    lv_obj_set_style_text_color(clock_date_label, lv_color_hex(0xcbd5e1), 0);
    lv_obj_set_style_text_align(clock_date_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(clock_date_label, &lv_font_montserrat_22, 0);
    clock_connection_led = lv_obj_create(screen);
    lv_obj_clear_flag(clock_connection_led, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(clock_connection_led, 16, 16);
    lv_obj_set_pos(clock_connection_led, (UI_WIDTH - 16) / 2, 276);
    lv_obj_set_style_radius(clock_connection_led, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(clock_connection_led, 2, 0);
    lv_obj_set_style_border_color(clock_connection_led, lv_color_hex(0xf8fafc), 0);
    lv_obj_set_style_border_opa(clock_connection_led, LV_OPA_50, 0);
    lv_anim_t led_animation;
    lv_anim_init(&led_animation);
    lv_anim_set_var(&led_animation, clock_connection_led);
    lv_anim_set_values(&led_animation, 5, 12);
    lv_anim_set_time(&led_animation, 1200);
    lv_anim_set_playback_time(&led_animation, 1200);
    lv_anim_set_repeat_count(&led_animation, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&led_animation, animate_connection_led);
    lv_anim_start(&led_animation);
    music_artwork = lv_obj_create(screen);
    lv_obj_clear_flag(music_artwork, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(music_artwork, 128, 128);
    lv_obj_set_pos(music_artwork, (UI_WIDTH - 128) / 2, 20);
    lv_obj_set_style_radius(music_artwork, 18, 0);
    lv_obj_set_style_bg_color(music_artwork, lv_color_hex(0x12352d), 0);
    lv_obj_set_style_border_width(music_artwork, 2, 0);
    lv_obj_set_style_border_color(music_artwork, lv_color_hex(0x34d399), 0);
    lv_obj_set_style_shadow_width(music_artwork, 12, 0);
    lv_obj_set_style_shadow_color(music_artwork, lv_color_hex(0x20d99a), 0);
    lv_obj_set_style_shadow_opa(music_artwork, LV_OPA_30, 0);
    music_record = lv_obj_create(music_artwork);
    lv_obj_clear_flag(music_record, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(music_record, 88, 88);
    lv_obj_center(music_record);
    lv_obj_set_style_radius(music_record, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(music_record, lv_color_hex(0x07130f), 0);
    lv_obj_set_style_border_width(music_record, 3, 0);
    lv_obj_set_style_border_color(music_record, lv_color_hex(0x285e4d), 0);
    lv_obj_t *record_label = lv_label_create(music_record);
    lv_label_set_text(record_label, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(record_label, lv_color_hex(0x6ee7b7), 0);
    lv_obj_set_style_text_font(record_label, &lv_font_montserrat_22, 0);
    lv_obj_center(record_label);
    music_album_art = lv_img_create(music_artwork);
    lv_obj_center(music_album_art);
    lv_obj_add_flag(music_album_art, LV_OBJ_FLAG_HIDDEN);
    music_track_label = lv_label_create(screen);
    lv_obj_set_width(music_track_label, UI_WIDTH);
    lv_obj_set_pos(music_track_label, 0, 166);
    lv_label_set_text(music_track_label, "Nothing playing");
    lv_obj_set_style_text_color(music_track_label, lv_color_hex(0xf8fafc), 0);
    lv_obj_set_style_text_align(music_track_label, LV_TEXT_ALIGN_CENTER, 0);
    music_message_label = lv_label_create(screen);
    lv_obj_set_width(music_message_label, UI_WIDTH);
    lv_obj_set_pos(music_message_label, 0, 190);
    lv_obj_set_style_text_color(music_message_label, lv_color_hex(0x94a3b8), 0);
    lv_obj_set_style_text_align(music_message_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(music_message_label, &lv_font_montserrat_14, 0);
    lv_obj_add_flag(music_message_label, LV_OBJ_FLAG_HIDDEN);
    music_controls = lv_obj_create(screen);
    lv_obj_clear_flag(music_controls, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(music_controls, UI_WIDTH, 72);
    lv_obj_set_pos(music_controls, 0, 216);
    lv_obj_set_style_bg_opa(music_controls, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(music_controls, 0, 0);
    lv_obj_set_style_pad_all(music_controls, 0, 0);
    create_music_control(music_controls, 56, 58, LV_SYMBOL_PREV, false, SATELLITE_SPOTIFY_PREVIOUS);
    create_music_control(music_controls, 144, 72, LV_SYMBOL_PLAY, true, SATELLITE_SPOTIFY_TOGGLE_PLAYBACK);
    create_music_control(music_controls, 246, 58, LV_SYMBOL_NEXT, false, SATELLITE_SPOTIFY_NEXT);
    set_datetime_visible(false);
    set_music_visible(false);
}

static uint16_t pulse_size(size_t index)
{
    const uint32_t phase = (lv_tick_get() / 12 + index * 29) % 100;
    const uint32_t pulse = phase < 50 ? phase : 100 - phase;
    return 90 + index * 48 + pulse / 2;
}

static void render(void)
{
    const bool active = assistant_state != SATELLITE_ASSISTANT_IDLE &&
                        assistant_state != SATELLITE_ASSISTANT_ERROR;
    if (idle_view_visible() &&
        (active || assistant_state == SATELLITE_ASSISTANT_ERROR ||
         (int32_t)(lv_tick_get() - clock_visible_until) >= 0)) {
        set_datetime_visible(false);
        set_music_visible(false);
    }
    const lv_color_t connection_led_color = connection_state == SATELLITE_CONNECTION_CONNECTED
                                                ? lv_color_hex(0x22c55e)
                                                : lv_color_hex(0xef4444);
    lv_obj_set_style_bg_color(clock_connection_led, connection_led_color, 0);
    lv_obj_set_style_shadow_color(clock_connection_led, connection_led_color, 0);
    for (size_t index = 0; index < 3; ++index) {
        if (active) {
            lv_obj_clear_flag(rings[index], LV_OBJ_FLAG_HIDDEN);
            const uint16_t size = pulse_size(index);
            lv_obj_set_size(rings[index], size, size);
            lv_obj_center(rings[index]);
            lv_obj_set_style_border_color(rings[index], lv_color_hex(0x20d99a), 0);
        } else {
            lv_obj_add_flag(rings[index], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (connection_status_suppressed || idle_view_visible() || active ||
        (satellite_protocol_ready() && connection_state == SATELLITE_CONNECTION_CONNECTED)) {
        lv_obj_add_flag(status_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        const char *status_text = "Starting";
        lv_color_t status_color = lv_color_hex(0x94a3b8);
        switch (connection_state) {
        case SATELLITE_CONNECTION_WIFI_CONNECTING:
            status_text = "Connecting Wi-Fi";
            break;
        case SATELLITE_CONNECTION_WIFI_READY:
            status_text = "Synchronizing clock";
            break;
        case SATELLITE_CONNECTION_SERVER_CONNECTING:
            status_text = "Connecting server";
            break;
        case SATELLITE_CONNECTION_CONNECTED:
            status_text = "Connected";
            status_color = lv_color_hex(0x20d99a);
            break;
        case SATELLITE_CONNECTION_ERROR:
            status_text = "Connection unavailable";
            status_color = lv_color_hex(0xff6b6b);
            break;
        default:
            break;
        }
        lv_label_set_text(status_label, status_text);
        lv_obj_set_style_text_color(status_label, status_color, 0);
        lv_obj_clear_flag(status_label, LV_OBJ_FLAG_HIDDEN);
    }
}

static void ui_task(void *argument)
{
    (void)argument;
    setup_lvgl();
    for (;;) {
        ui_event_t event;
        int assistant_event;
        satellite_connection_status_t connection_event;
        bool show_wake_immediately = false;
        BaseType_t received = xQueueReceive(ui_events, &event, pdMS_TO_TICKS(20));
        while (received == pdPASS) {
            if (event.type == UI_EVENT_ASSISTANT) {
                assistant_state = event.value;
                show_wake_immediately |= assistant_state == SATELLITE_ASSISTANT_WAKE_DETECTED;
            }
            if (event.type == UI_EVENT_MUSIC) {
                music_playing = event.value != 0;
                lv_label_set_text(music_track_label, event.track);
                lv_label_set_text(music_play_icon, music_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
            }
            if (event.type == UI_EVENT_MUSIC_MESSAGE) {
                lv_label_set_text(music_message_label, event.track);
                if (event.track[0] == '\0' || lv_obj_has_flag(music_artwork, LV_OBJ_FLAG_HIDDEN)) {
                    lv_obj_add_flag(music_message_label, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_clear_flag(music_message_label, LV_OBJ_FLAG_HIDDEN);
                }
            }
            if (event.type == UI_EVENT_ALBUM_ART) {
                const lv_img_dsc_t *album_art = satellite_spotify_album_art();
                const lv_coord_t largest_edge = album_art->header.w > album_art->header.h
                                                    ? album_art->header.w
                                                    : album_art->header.h;
                lv_img_set_src(music_album_art, album_art);
                lv_img_set_pivot(music_album_art, album_art->header.w / 2, album_art->header.h / 2);
                lv_img_set_zoom(music_album_art, 120 * 256 / largest_edge);
                lv_obj_clear_flag(music_album_art, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(music_record, LV_OBJ_FLAG_HIDDEN);
            }
            received = xQueueReceive(ui_events, &event, 0);
        }
        while (xQueueReceive(assistant_events, &assistant_event, 0) == pdPASS) {
            assistant_state = assistant_event;
            show_wake_immediately |= assistant_state == SATELLITE_ASSISTANT_WAKE_DETECTED;
        }
        while (xQueueReceive(connection_events, &connection_event, 0) == pdPASS) {
            connection_state = connection_event;
            connection_status_suppressed |= connection_state == SATELLITE_CONNECTION_UNAVAILABLE;
        }
        render();
        if (show_wake_immediately) {
            lv_refr_now(NULL);
        }
        lv_timer_handler();
    }
}

esp_err_t satellite_ui_start(void)
{
    if (ui_events != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(waveshare_touch_lcd_185c_init_display(), TAG, "Display initialization failed");
    ui_events = xQueueCreate(UI_EVENT_QUEUE_DEPTH, sizeof(ui_event_t));
    assistant_events = xQueueCreate(UI_ASSISTANT_EVENT_QUEUE_DEPTH, sizeof(int));
    connection_events = xQueueCreate(UI_CONNECTION_EVENT_QUEUE_DEPTH, sizeof(satellite_connection_status_t));
    if (ui_events == NULL || assistant_events == NULL || connection_events == NULL ||
        xTaskCreate(ui_task, "ui", 6144, NULL, 4, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    const esp_timer_create_args_t tick_timer_config = {
        .callback = lvgl_tick,
        .name = "lvgl_tick",
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_timer_config, &lvgl_tick_timer), TAG, "LVGL tick timer creation failed");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(lvgl_tick_timer, 5000), TAG, "LVGL tick timer start failed");
    return ESP_OK;
}

void satellite_ui_post_connection_status(satellite_connection_status_t status)
{
    if (connection_events != NULL) {
        xQueueOverwrite(connection_events, &status);
    }
}

void satellite_ui_post_assistant_state(int state)
{
    if (assistant_events != NULL) {
        xQueueOverwrite(assistant_events, &state);
    }
}

void satellite_ui_post_music_state(const char *track, bool playing)
{
    if (ui_events != NULL) {
        ui_event_t event = {.type = UI_EVENT_MUSIC, .value = playing};
        strlcpy(event.track, track, sizeof(event.track));
        xQueueSend(ui_events, &event, 0);
    }
}

void satellite_ui_post_music_message(const char *message)
{
    if (ui_events != NULL) {
        ui_event_t event = {.type = UI_EVENT_MUSIC_MESSAGE};
        strlcpy(event.track, message, sizeof(event.track));
        xQueueSend(ui_events, &event, 0);
    }
}

void satellite_ui_post_album_art(void)
{
    if (ui_events != NULL) {
        const ui_event_t event = {.type = UI_EVENT_ALBUM_ART};
        xQueueSend(ui_events, &event, 0);
    }
}
