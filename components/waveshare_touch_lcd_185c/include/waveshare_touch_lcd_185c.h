#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define FULLOCH_AUDIO_FRAME_SAMPLES 320
#define FULLOCH_AEC_CAPTURE_CHANNELS 4
#define FULLOCH_AEC_CAPTURE_MAX_SAMPLES 1024

esp_err_t waveshare_touch_lcd_185c_init_capture(void);
esp_err_t waveshare_touch_lcd_185c_set_backlight(uint8_t percent);
esp_err_t waveshare_touch_lcd_185c_init_display(void);
esp_err_t waveshare_touch_lcd_185c_fill_display(uint16_t rgb565);
esp_err_t waveshare_touch_lcd_185c_draw_rgb565(uint16_t x1, uint16_t y1, uint16_t x2, uint16_t y2,
                                                const uint16_t *pixels);
bool waveshare_touch_lcd_185c_read_touch(uint16_t *x, uint16_t *y);
esp_err_t waveshare_touch_lcd_185c_init_playback(void);
esp_err_t waveshare_touch_lcd_185c_start_playback(void);
esp_err_t waveshare_touch_lcd_185c_set_playback_volume(uint8_t percent);
esp_err_t waveshare_touch_lcd_185c_play_mono_s16le(const int16_t *samples, size_t sample_count);
esp_err_t waveshare_touch_lcd_185c_play_silence(size_t sample_count);
esp_err_t waveshare_touch_lcd_185c_stop_playback(void);
esp_err_t waveshare_touch_lcd_185c_read_mono_s16le(int16_t output[FULLOCH_AUDIO_FRAME_SAMPLES]);
esp_err_t waveshare_touch_lcd_185c_read_aec_input(int16_t *output, size_t sample_count);
