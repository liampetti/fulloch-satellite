#include "driver/gpio.h"
#include "audio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "connectivity.h"
#include "ui.h"
#include "waveshare_touch_lcd_185c.h"

static const char *TAG = "fulloch_satellite";

void app_main(void)
{
    ESP_LOGW(TAG, "Reset reason: %d", esp_reset_reason());
    ESP_LOGI(TAG, "Fulloch satellite starting for %s", FULLOCH_BOARD);
    ESP_LOGI(TAG, "ESP-IDF %s", esp_get_idf_version());
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(gpio_set_direction(GPIO_NUM_15, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_15, 0));
    ESP_ERROR_CHECK(waveshare_touch_lcd_185c_init_playback());
    ESP_ERROR_CHECK(satellite_audio_start());
    ESP_ERROR_CHECK(satellite_ui_start());
    ESP_ERROR_CHECK(satellite_connectivity_start());
}
