#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "audio.h"
#include "esp_afe_sr_models.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "protocol.h"
#include "waveshare_touch_lcd_185c.h"

#define UPLINK_QUEUE_DEPTH 8
#define DOWNLINK_QUEUE_DEPTH 256
#define DOWNLINK_MAX_SAMPLES 2048
#define PLAYBACK_PREROLL_MS 120
#define PLAYBACK_PREROLL_POLL_MS 10
#define PLAYBACK_QUEUE_WAIT_MS 5
#define PLAYBACK_IDLE_MS 30
#define PLAYBACK_BLOCK_SAMPLES 128
#define UPLINK_FRAME_INTERVAL_US 20000
#define AFE_INPUT_FORMAT "RMNM"
#define AFE_FILTER_LENGTH 4
#define AFE_CAPTURE_TASK_PRIORITY 6
#define AFE_FETCH_TASK_PRIORITY 5
#define AFE_CAPTURE_CORE 1
#define UPLINK_TASK_PRIORITY 6
#define AUDIO_IO_CORE 0

typedef struct {
    uint32_t generation;
    int16_t samples[FULLOCH_AUDIO_FRAME_SAMPLES];
} uplink_frame_t;

typedef struct {
    uint32_t generation;
    bool sequenced;
    uint32_t sequence;
    size_t sample_count;
    int16_t samples[DOWNLINK_MAX_SAMPLES];
} downlink_frame_t;

static QueueHandle_t uplink_queue;
static QueueHandle_t downlink_queue;
static QueueHandle_t downlink_free_queue;
static bool aec_requested;
static volatile bool aec_enabled;
static const esp_afe_sr_iface_t *afe_handle;
static esp_afe_sr_data_t *afe_data;
static int afe_feed_samples;
static int afe_feed_channels;
static int afe_fetch_samples;
static int16_t *afe_input;
static int16_t uplink_reframe[FULLOCH_AUDIO_FRAME_SAMPLES];
static size_t uplink_reframe_samples;
static volatile uint32_t uplink_generation;
static volatile uint32_t playback_generation;
static volatile bool playback_active;
static satellite_audio_metrics_t metrics;
static downlink_frame_t *downlink_pool;
static downlink_frame_t *downlink_input;
static uint32_t downlink_chunks_received;
static uint32_t playback_chunks_written;
static uint32_t downlink_samples_received;
static uint32_t downlink_queue_peak;
static uint32_t playback_gap_timeouts;
static int64_t last_downlink_received_us;
static int64_t downlink_gap_min_us;
static int64_t downlink_gap_max_us;
static int64_t playback_write_total_us;
static int64_t playback_write_max_us;
static uint32_t afe_fetch_blocks;
static uint32_t afe_fetch_failures;

static void reset_playback_diagnostics(void)
{
    downlink_chunks_received = 0;
    playback_chunks_written = 0;
    downlink_samples_received = 0;
    downlink_queue_peak = 0;
    playback_gap_timeouts = 0;
    last_downlink_received_us = 0;
    downlink_gap_min_us = 0;
    downlink_gap_max_us = 0;
    playback_write_total_us = 0;
    playback_write_max_us = 0;
}

static void discard_queued_downlink(void)
{
    if (downlink_queue == NULL || downlink_free_queue == NULL) {
        return;
    }
    downlink_frame_t *frame;
    while (xQueueReceive(downlink_queue, &frame, 0) == pdPASS) {
        if (xQueueSend(downlink_free_queue, &frame, 0) != pdPASS) {
            ESP_LOGE("audio", "Unable to return discarded downlink frame");
        }
    }
}

static void discard_queued_uplink(void)
{
    uplink_generation++;
    uplink_reframe_samples = 0;
    if (uplink_queue != NULL) {
        xQueueReset(uplink_queue);
    }
}

static void log_afe_output_levels(const int16_t *samples, size_t sample_count)
{
    if (++afe_fetch_blocks > 5 && afe_fetch_blocks % 100 != 0) {
        return;
    }
    int16_t minimum = samples[0];
    int16_t maximum = samples[0];
    for (size_t sample = 1; sample < sample_count; ++sample) {
        if (samples[sample] < minimum) {
            minimum = samples[sample];
        }
        if (samples[sample] > maximum) {
            maximum = samples[sample];
        }
    }
    ESP_LOGI("audio", "AFE output: %u samples, range=%d..%d", (unsigned int)sample_count, minimum, maximum);
}

static esp_err_t configure_playback_volume(void)
{
    char configured_volume[4];
    size_t configured_volume_size = sizeof(configured_volume);
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open("audio", NVS_READONLY, &handle), "audio", "Audio config unavailable");
    esp_err_t error = nvs_get_str(handle, "speaker_volume", configured_volume, &configured_volume_size);
    nvs_close(handle);
    ESP_RETURN_ON_ERROR(error, "audio", "Speaker volume unavailable");

    char *end;
    const long volume = strtol(configured_volume, &end, 10);
    ESP_RETURN_ON_FALSE(end != configured_volume && *end == '\0' && volume >= 0 && volume <= 100,
                        ESP_ERR_INVALID_ARG, "audio", "Invalid speaker volume");
    ESP_RETURN_ON_ERROR(waveshare_touch_lcd_185c_set_playback_volume((uint8_t)volume), "audio",
                        "Speaker volume setup failed");
    ESP_LOGI("audio", "Speaker volume set to %ld%%", volume);
    return ESP_OK;
}

static esp_err_t load_aec_enabled(void)
{
    char configured[6];
    size_t configured_size = sizeof(configured);
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open("audio", NVS_READONLY, &handle), "audio", "Audio config unavailable");
    const esp_err_t error = nvs_get_str(handle, "aec_enabled", configured, &configured_size);
    nvs_close(handle);
    ESP_RETURN_ON_ERROR(error, "audio", "AEC setting unavailable");
    if (strcmp(configured, "true") == 0) {
        aec_requested = true;
    } else if (strcmp(configured, "false") == 0) {
        aec_requested = false;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static void capture_task(void *argument)
{
    (void)argument;
    for (;;) {
        if (!aec_enabled) {
            uplink_frame_t frame = {
                .generation = uplink_generation,
            };
            if (waveshare_touch_lcd_185c_read_mono_s16le(frame.samples) != ESP_OK) {
                metrics.capture_overruns++;
            } else if (satellite_protocol_ready() && xQueueSend(uplink_queue, &frame, 0) != pdPASS) {
                metrics.dropped_uplink_frames++;
            }
            continue;
        }
        if (waveshare_touch_lcd_185c_read_aec_input(afe_input, afe_feed_samples) != ESP_OK) {
            metrics.capture_overruns++;
            continue;
        }
        if (afe_handle->feed(afe_data, afe_input) <= 0) {
            metrics.capture_overruns++;
        }
    }
}

static void afe_fetch_task(void *argument)
{
    (void)argument;
    for (;;) {
        if (!aec_enabled) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        afe_fetch_result_t *result = afe_handle->fetch(afe_data);
        if (result == NULL || result->ret_value != ESP_OK || result->data == NULL || result->data_size <= 0 ||
            (result->data_size % sizeof(*result->data)) != 0) {
            if (++afe_fetch_failures <= 5 || afe_fetch_failures % 100 == 0) {
                ESP_LOGW("audio", "AFE fetch unavailable: result=%p status=%d", result,
                         result == NULL ? ESP_FAIL : result->ret_value);
            }
            continue;
        }
        const size_t sample_count = (size_t)result->data_size / sizeof(*result->data);
        log_afe_output_levels(result->data, sample_count);
        for (size_t sample = 0; sample < sample_count; ++sample) {
            uplink_reframe[uplink_reframe_samples++] = result->data[sample];
            if (uplink_reframe_samples != FULLOCH_AUDIO_FRAME_SAMPLES) {
                continue;
            }
            uplink_frame_t frame = {
                .generation = uplink_generation,
            };
            memcpy(frame.samples, uplink_reframe, sizeof(frame.samples));
            uplink_reframe_samples = 0;
            if (satellite_protocol_ready() && xQueueSend(uplink_queue, &frame, 0) != pdPASS) {
                metrics.dropped_uplink_frames++;
            }
        }
    }
}

static void uplink_task(void *argument)
{
    (void)argument;
    uplink_frame_t frame;
    int64_t last_sent_us = 0;

    for (;;) {
        if (xQueueReceive(uplink_queue, &frame, portMAX_DELAY) != pdPASS) {
            continue;
        }
        if (!satellite_protocol_ready()) {
            continue;
        }
        if (frame.generation != uplink_generation) {
            continue;
        }
        if (!aec_enabled && (satellite_protocol_assistant_state() == SATELLITE_ASSISTANT_SPEAKING ||
                             satellite_audio_playback_pending())) {
            continue;
        }
        if (last_sent_us != 0) {
            const int64_t remaining_us = last_sent_us + UPLINK_FRAME_INTERVAL_US - esp_timer_get_time();
            if (remaining_us > 0) {
                vTaskDelay(pdMS_TO_TICKS((remaining_us + 999) / 1000));
            }
        }
        last_sent_us = esp_timer_get_time();
        if (satellite_protocol_send_uplink(frame.samples, FULLOCH_AUDIO_FRAME_SAMPLES) != ESP_OK) {
            metrics.dropped_uplink_frames++;
        }
    }
}

static void downlink_playback_task(void *argument)
{
    (void)argument;
    bool playback_started = false;
    int64_t last_audio_us = 0;
    for (;;) {
        if (xQueueReceive(downlink_queue, &downlink_input,
                          pdMS_TO_TICKS(PLAYBACK_QUEUE_WAIT_MS)) != pdPASS) {
            if (playback_started) {
                playback_gap_timeouts++;
                if (waveshare_touch_lcd_185c_play_silence(PLAYBACK_BLOCK_SAMPLES) != ESP_OK) {
                    metrics.playback_underruns++;
                }
                if (esp_timer_get_time() - last_audio_us >= PLAYBACK_IDLE_MS * 1000) {
                    playback_started = false;
                    playback_active = false;
                }
            }
            continue;
        }
        if (downlink_input->generation != playback_generation) {
            xQueueSend(downlink_free_queue, &downlink_input, 0);
            continue;
        }
        if (downlink_input->generation == playback_generation) {
            if (!playback_started) {
                const int64_t preroll_deadline_us = esp_timer_get_time() + PLAYBACK_PREROLL_MS * 1000;
                while (uxQueueMessagesWaiting(downlink_queue) == 0 &&
                       esp_timer_get_time() < preroll_deadline_us) {
                    vTaskDelay(pdMS_TO_TICKS(PLAYBACK_PREROLL_POLL_MS));
                }
                playback_started = true;
            }
            playback_active = true;
            playback_chunks_written++;
            if (playback_chunks_written <= 8) {
                ESP_LOGI("audio", "Writing playback chunk %lu: %u samples", (unsigned long)playback_chunks_written,
                         (unsigned int)downlink_input->sample_count);
            }
            const int64_t write_started_us = esp_timer_get_time();
            if (waveshare_touch_lcd_185c_play_mono_s16le(downlink_input->samples,
                                                          downlink_input->sample_count) != ESP_OK) {
                metrics.playback_underruns++;
            } else if (downlink_input->sequenced) {
                // Only acknowledge a sequence once all of its PCM reached I2S.
                satellite_protocol_playback_frame_completed(downlink_input->sequence);
            }
            const int64_t write_duration_us = esp_timer_get_time() - write_started_us;
            playback_write_total_us += write_duration_us;
            if (write_duration_us > playback_write_max_us) {
                playback_write_max_us = write_duration_us;
            }
            last_audio_us = esp_timer_get_time();
        }
        xQueueSend(downlink_free_queue, &downlink_input, 0);
    }
}

esp_err_t satellite_audio_start(void)
{
    if (uplink_queue != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_RETURN_ON_ERROR(configure_playback_volume(), "audio", "Playback configuration failed");
    ESP_RETURN_ON_ERROR(load_aec_enabled(), "audio", "AEC configuration failed");
    ESP_RETURN_ON_ERROR(waveshare_touch_lcd_185c_init_capture(), "audio", "Capture initialization failed");
    if (aec_requested) {
        ESP_LOGI("audio", "AEC requested; activation deferred until server connection completes");
    } else {
        ESP_LOGW("audio", "AEC disabled by provisioning; full-duplex echo suppression is unavailable");
    }
    downlink_pool = heap_caps_calloc(DOWNLINK_QUEUE_DEPTH, sizeof(*downlink_pool), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uplink_queue = xQueueCreate(UPLINK_QUEUE_DEPTH, sizeof(uplink_frame_t));
    downlink_queue = xQueueCreate(DOWNLINK_QUEUE_DEPTH, sizeof(downlink_frame_t *));
    downlink_free_queue = xQueueCreate(DOWNLINK_QUEUE_DEPTH, sizeof(downlink_frame_t *));
    if (downlink_pool == NULL || uplink_queue == NULL ||
        downlink_queue == NULL || downlink_free_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    for (size_t index = 0; index < DOWNLINK_QUEUE_DEPTH; ++index) {
        downlink_frame_t *frame = &downlink_pool[index];
        if (xQueueSend(downlink_free_queue, &frame, 0) != pdPASS) {
            return ESP_FAIL;
        }
    }
    if (xTaskCreatePinnedToCore(capture_task, "capture", 8192, NULL, AFE_CAPTURE_TASK_PRIORITY, NULL,
                                AFE_CAPTURE_CORE) != pdPASS ||
        xTaskCreatePinnedToCore(afe_fetch_task, "afe_fetch", 4096, NULL, AFE_FETCH_TASK_PRIORITY, NULL,
                                AFE_CAPTURE_CORE) != pdPASS ||
        xTaskCreatePinnedToCore(uplink_task, "uplink", 4096, NULL, UPLINK_TASK_PRIORITY, NULL,
                                AUDIO_IO_CORE) != pdPASS ||
        xTaskCreatePinnedToCore(downlink_playback_task, "downlink", 4096, NULL, 5, NULL,
                                AUDIO_IO_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t satellite_audio_enable_aec(void)
{
    if (!aec_requested || aec_enabled) {
        return ESP_OK;
    }
    afe_config_t *config = afe_config_init(AFE_INPUT_FORMAT, NULL, AFE_TYPE_VC, AFE_MODE_LOW_COST);
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_NO_MEM, "audio", "AFE configuration failed");
    config->aec_init = true;
    config->aec_mode = AEC_MODE_VOIP_LOW_COST;
    config->aec_filter_length = AFE_FILTER_LENGTH;
    // Aggressive NLP suppressed near-end speech with the board's RMNM reference.
    config->aec_nlp_level = AEC_NLP_LEVEL_NORMAL;
    config->ns_init = false;
    config->vad_init = false;
    config->wakenet_init = false;
    config->agc_init = false;
    config->se_init = false;
    config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    config->afe_perferred_core = AFE_CAPTURE_CORE;
    config->afe_perferred_priority = AFE_FETCH_TASK_PRIORITY;
    config->fixed_output_channel = true;
    config = afe_config_check(config);
    const esp_afe_sr_iface_t *handle = esp_afe_handle_from_config(config);
    esp_afe_sr_data_t *data = handle == NULL ? NULL : handle->create_from_config(config);
    afe_config_free(config);
    ESP_RETURN_ON_FALSE(handle != NULL && data != NULL, ESP_ERR_NO_MEM, "audio", "AFE initialization failed");
    const int feed_samples = handle->get_feed_chunksize(data);
    const int feed_channels = handle->get_feed_channel_num(data);
    const int fetch_samples = handle->get_fetch_chunksize(data);
    if (feed_samples <= 0 || feed_samples > FULLOCH_AEC_CAPTURE_MAX_SAMPLES || fetch_samples <= 0 ||
        feed_channels != FULLOCH_AEC_CAPTURE_CHANNELS) {
        ESP_LOGE("audio", "Unsupported AFE format: feed=%d fetch=%d channels=%d", feed_samples, fetch_samples,
                 feed_channels);
        handle->destroy(data);
        return ESP_FAIL;
    }
    int16_t *input = heap_caps_aligned_alloc(16, (size_t)feed_samples * (size_t)feed_channels * sizeof(*input),
                                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (input == NULL) {
        handle->destroy(data);
        free(input);
        return ESP_ERR_NO_MEM;
    }
    afe_handle = handle;
    afe_data = data;
    afe_feed_samples = feed_samples;
    afe_feed_channels = feed_channels;
    afe_fetch_samples = fetch_samples;
    afe_input = input;
    aec_enabled = true;
    ESP_LOGI("audio", "AFE enabled: feed=%d fetch=%d samples channels=%d", afe_feed_samples,
             afe_fetch_samples, afe_feed_channels);
    afe_handle->print_pipeline(afe_data);
    return ESP_OK;
}

bool satellite_audio_aec_active(void)
{
    return aec_enabled;
}

void satellite_audio_reset(void)
{
    satellite_audio_discard_uplink();
    if (aec_enabled) {
        afe_handle->reset_buffer(afe_data);
    }
    playback_generation++;
    reset_playback_diagnostics();
    discard_queued_downlink();
    playback_active = false;
}

void satellite_audio_discard_uplink(void)
{
    discard_queued_uplink();
}

void satellite_audio_cancel_playback(void)
{
    // Disable the amplifier before discarding frames so a barge-in cuts TTS
    // immediately, including audio already handed to the I2S DMA engine.
    waveshare_touch_lcd_185c_stop_playback();
    satellite_audio_log_playback_diagnostics();
    playback_generation++;
    reset_playback_diagnostics();
    discard_queued_downlink();
    playback_active = false;
}

void satellite_audio_finish_playback(void)
{
    // Called only after the downlink queue and I2S path have drained. Unlike a
    // cancel, preserve queued audio until it has completed before powering down.
    waveshare_touch_lcd_185c_stop_playback();
    satellite_audio_log_playback_diagnostics();
    playback_active = false;
}

void satellite_audio_begin_playback(void)
{
    reset_playback_diagnostics();
    if (waveshare_touch_lcd_185c_start_playback() != ESP_OK) {
        ESP_LOGW("audio", "Unable to enable playback amplifier");
    }
}

void satellite_audio_log_playback_diagnostics(void)
{
    const int64_t average_write_us = playback_chunks_written == 0
                                         ? 0
                                         : playback_write_total_us / playback_chunks_written;
    ESP_LOGI("audio",
             "Playback diagnostics: frames=%" PRIu32 " samples=%" PRIu32
             " gaps_us=%" PRId64 "..%" PRId64 " queue_peak=%" PRIu32
             " gap_timeouts=%" PRIu32 " writes=%" PRIu32
             " write_us_avg=%" PRId64 " write_us_max=%" PRId64,
             downlink_chunks_received, downlink_samples_received, downlink_gap_min_us,
             downlink_gap_max_us, downlink_queue_peak, playback_gap_timeouts,
             playback_chunks_written, average_write_us, playback_write_max_us);
}

bool satellite_audio_enqueue_downlink(const int16_t *samples, size_t sample_count,
                                      bool sequenced, uint32_t sequence)
{
    if (downlink_queue == NULL || downlink_free_queue == NULL || samples == NULL || sample_count == 0 ||
        sample_count > DOWNLINK_MAX_SAMPLES) {
        return false;
    }
    downlink_frame_t *frame;
    if (xQueueReceive(downlink_free_queue, &frame, 0) != pdPASS) {
        metrics.dropped_downlink_frames++;
        return false;
    }
    frame->generation = playback_generation;
    frame->sequenced = sequenced;
    frame->sequence = sequence;
    frame->sample_count = sample_count;
    memcpy(frame->samples, samples, sample_count * sizeof(*samples));
    const int64_t now_us = esp_timer_get_time();
    if (last_downlink_received_us != 0) {
        const int64_t gap_us = now_us - last_downlink_received_us;
        if (downlink_gap_min_us == 0 || gap_us < downlink_gap_min_us) {
            downlink_gap_min_us = gap_us;
        }
        if (gap_us > downlink_gap_max_us) {
            downlink_gap_max_us = gap_us;
        }
    }
    last_downlink_received_us = now_us;
    downlink_samples_received += sample_count;
    downlink_chunks_received++;
    if (downlink_chunks_received <= 8) {
        int16_t minimum = frame->samples[0];
        int16_t maximum = frame->samples[0];
        for (size_t sample = 1; sample < sample_count; ++sample) {
            if (frame->samples[sample] < minimum) {
                minimum = frame->samples[sample];
            }
            if (frame->samples[sample] > maximum) {
                maximum = frame->samples[sample];
            }
        }
        ESP_LOGI("audio", "Received downlink chunk %lu: %u samples, range=%d..%d",
                 (unsigned long)downlink_chunks_received, (unsigned int)sample_count, minimum, maximum);
    }
    if (xQueueSend(downlink_queue, &frame, 0) != pdPASS) {
        metrics.dropped_downlink_frames++;
        xQueueSend(downlink_free_queue, &frame, 0);
        return false;
    }
    const uint32_t queue_depth = uxQueueMessagesWaiting(downlink_queue);
    if (queue_depth > downlink_queue_peak) {
        downlink_queue_peak = queue_depth;
    }
    return true;
}

satellite_audio_metrics_t satellite_audio_metrics(void)
{
    return metrics;
}

bool satellite_audio_playback_pending(void)
{
    return playback_active || (downlink_queue != NULL && uxQueueMessagesWaiting(downlink_queue) != 0);
}
