#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint32_t dropped_uplink_frames;
    uint32_t dropped_downlink_frames;
    uint32_t capture_overruns;
    uint32_t playback_underruns;
} satellite_audio_metrics_t;

esp_err_t satellite_audio_start(void);
esp_err_t satellite_audio_enable_aec(void);
bool satellite_audio_aec_active(void);
void satellite_audio_reset(void);
void satellite_audio_discard_uplink(void);
void satellite_audio_cancel_playback(void);
void satellite_audio_begin_playback(void);
void satellite_audio_log_playback_diagnostics(void);
bool satellite_audio_enqueue_downlink(const int16_t *samples, size_t sample_count);
satellite_audio_metrics_t satellite_audio_metrics(void);
bool satellite_audio_playback_pending(void);
