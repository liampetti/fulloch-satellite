#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "audio.h"
#include "esp_aec.h"
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
#define UPLINK_MAX_SAMPLES (FULLOCH_AUDIO_FRAME_SAMPLES * 2)
#define AFE_INPUT_FORMAT "RMNM"
#define AFE_FILTER_LENGTH 4
#define AFE_CAPTURE_TASK_PRIORITY 6
#define AFE_FETCH_TASK_PRIORITY 5
#define AFE_CAPTURE_CORE 1
#define UPLINK_TASK_PRIORITY 6
#define AUDIO_IO_CORE 0

typedef struct {
    uint32_t generation;
    int16_t samples[UPLINK_MAX_SAMPLES];
} uplink_frame_t;

typedef struct {
    uint32_t generation;
    bool sequenced;
    uint32_t sequence;
    size_t sample_count;
    int16_t samples[DOWNLINK_MAX_SAMPLES];
} downlink_frame_t;

static QueueHandle_t uplink_queue;
static uplink_frame_t uplink_output;
static QueueHandle_t downlink_queue;
static QueueHandle_t downlink_free_queue;
static TaskHandle_t capture_task_handle;
static TaskHandle_t afe_fetch_task_handle;
static TaskHandle_t uplink_task_handle;
static TaskHandle_t downlink_task_handle;
static bool aec_requested;
static volatile bool aec_enabled;
static uint8_t supported_uplink_channels = 1;
static uint8_t active_uplink_channels = 1;
static const esp_afe_sr_iface_t *afe_handle;
static esp_afe_sr_data_t *afe_data;
static int afe_feed_samples;
static int afe_feed_channels;
static int afe_fetch_samples;
static int16_t *afe_input;
static aec_handle_t *direct_aec;
static int direct_aec_samples;
static int16_t *direct_aec_capture;
static int16_t *direct_aec_microphones;
static int16_t *direct_aec_reference;
static int16_t *direct_aec_output;
static int16_t uplink_reframe[UPLINK_MAX_SAMPLES];
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

static void log_audio_output_levels(const int16_t *samples, size_t sample_count)
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
    ESP_LOGI("audio", "AEC output: %u samples, range=%d..%d [internal=%u/%u psram=%u/%u]",
             (unsigned int)sample_count, minimum, maximum,
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned int)heap_caps_get_total_size(MALLOC_CAP_INTERNAL),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned int)heap_caps_get_total_size(MALLOC_CAP_SPIRAM));
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

static esp_err_t load_uplink_channels(void)
{
    char configured[2];
    size_t configured_size = sizeof(configured);
    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open("audio", NVS_READONLY, &handle), "audio", "Audio config unavailable");
    const esp_err_t error = nvs_get_str(handle, "uplink_channels", configured, &configured_size);
    nvs_close(handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        supported_uplink_channels = 1;
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(error, "audio", "Uplink channel setting unavailable");
    if (strcmp(configured, "1") == 0) {
        supported_uplink_channels = 1;
    } else if (strcmp(configured, "2") == 0 && aec_requested) {
        supported_uplink_channels = 2;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static bool queue_uplink_frame(const int16_t *samples, size_t sample_count)
{
    uplink_frame_t frame = {
        .generation = uplink_generation,
    };
    memcpy(frame.samples, samples, sample_count * sizeof(*samples));
    if (satellite_protocol_ready() && xQueueSend(uplink_queue, &frame, 0) != pdPASS) {
        metrics.dropped_uplink_frames++;
        return false;
    }
    return true;
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
        if (direct_aec != NULL) {
            if (waveshare_touch_lcd_185c_read_aec_input(direct_aec_capture, direct_aec_samples) != ESP_OK) {
                metrics.capture_overruns++;
                continue;
            }
            for (int sample = 0; sample < direct_aec_samples; ++sample) {
                // The hardware RMNM slots provide one synchronous reference for both microphones.
                direct_aec_reference[sample] = direct_aec_capture[sample * FULLOCH_AEC_CAPTURE_CHANNELS];
                direct_aec_microphones[sample] = direct_aec_capture[sample * FULLOCH_AEC_CAPTURE_CHANNELS + 1];
                direct_aec_microphones[direct_aec_samples + sample] =
                    direct_aec_capture[sample * FULLOCH_AEC_CAPTURE_CHANNELS + 3];
            }
            aec_process(direct_aec, direct_aec_microphones, direct_aec_reference, direct_aec_output);
            log_audio_output_levels(direct_aec_output, (size_t)direct_aec_samples * 2);
            for (int sample = 0; sample < direct_aec_samples; ++sample) {
                uplink_reframe[uplink_reframe_samples++] = direct_aec_output[sample];
                uplink_reframe[uplink_reframe_samples++] = direct_aec_output[direct_aec_samples + sample];
                if (uplink_reframe_samples == UPLINK_MAX_SAMPLES) {
                    queue_uplink_frame(uplink_reframe, uplink_reframe_samples);
                    uplink_reframe_samples = 0;
                }
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
        if (direct_aec != NULL) {
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
        log_audio_output_levels(result->data, sample_count);
        for (size_t sample = 0; sample < sample_count; ++sample) {
            uplink_reframe[uplink_reframe_samples++] = result->data[sample];
            if (uplink_reframe_samples != FULLOCH_AUDIO_FRAME_SAMPLES) {
                continue;
            }
            uplink_frame_t frame = {
                .generation = uplink_generation,
            };
            memcpy(frame.samples, uplink_reframe, FULLOCH_AUDIO_FRAME_SAMPLES * sizeof(*frame.samples));
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
    int64_t last_sent_us = 0;

    for (;;) {
        if (xQueueReceive(uplink_queue, &uplink_output, portMAX_DELAY) != pdPASS) {
            continue;
        }
        if (!satellite_protocol_ready()) {
            continue;
        }
        if (uplink_output.generation != uplink_generation) {
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
        const size_t sample_count = FULLOCH_AUDIO_FRAME_SAMPLES * active_uplink_channels;
        if (satellite_protocol_send_uplink(uplink_output.samples, sample_count) != ESP_OK) {
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
                // A consecutive server turn can arrive after the prior turn
                // powered down the amplifier without an idle-to-speaking edge.
                // Enable it here as well as on that state transition so this
                // preroll always warms the analogue path.
                if (waveshare_touch_lcd_185c_start_playback() != ESP_OK) {
                    ESP_LOGW("audio", "Unable to enable playback amplifier");
                }
                const int64_t preroll_deadline_us = esp_timer_get_time() + PLAYBACK_PREROLL_MS * 1000;
                // The amplifier is enabled when speaking starts. Keep it on for
                // the full warm-up interval before its first I2S samples arrive.
                while (esp_timer_get_time() < preroll_deadline_us) {
                    vTaskDelay(pdMS_TO_TICKS(PLAYBACK_PREROLL_POLL_MS));
                }
                // Cancellation can occur while the analogue path warms. Do
                // not revive an invalidated frame after the preroll delay.
                if (downlink_input->generation != playback_generation) {
                    xQueueSend(downlink_free_queue, &downlink_input, 0);
                    continue;
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
    ESP_RETURN_ON_ERROR(load_uplink_channels(), "audio", "Uplink channel configuration failed");
    ESP_RETURN_ON_ERROR(waveshare_touch_lcd_185c_init_capture(), "audio", "Capture initialization failed");
    if (aec_requested) {
        ESP_LOGI("audio", "AEC requested; activation begins when the server connection completes");
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
    if (xTaskCreatePinnedToCore(capture_task, "capture", 8192, NULL, AFE_CAPTURE_TASK_PRIORITY, &capture_task_handle,
                                AFE_CAPTURE_CORE) != pdPASS ||
        xTaskCreatePinnedToCore(afe_fetch_task, "afe_fetch", 4096, NULL, AFE_FETCH_TASK_PRIORITY, &afe_fetch_task_handle,
                                AFE_CAPTURE_CORE) != pdPASS ||
        xTaskCreatePinnedToCore(uplink_task, "uplink", 4096, NULL, UPLINK_TASK_PRIORITY, &uplink_task_handle,
                                AUDIO_IO_CORE) != pdPASS ||
        xTaskCreatePinnedToCore(downlink_playback_task, "downlink", 4096, NULL, 5, &downlink_task_handle,
                                AUDIO_IO_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

uint8_t satellite_audio_supported_uplink_channels(void)
{
    return supported_uplink_channels;
}

uint8_t satellite_audio_active_uplink_channels(void)
{
    return active_uplink_channels;
}

esp_err_t satellite_audio_enable_aec(uint8_t uplink_channels)
{
    if (uplink_channels == 0 || uplink_channels > supported_uplink_channels ||
        (uplink_channels == 2 && !aec_requested)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (aec_enabled) {
        return active_uplink_channels == uplink_channels ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    active_uplink_channels = uplink_channels;
    if (!aec_requested) {
        return ESP_OK;
    }
    if (uplink_channels == 2) {
        aec_config_t config = {
            .mic_num = 2,
            .ref_num = 1,
            .out_num = 2,
            .filter_length = AFE_FILTER_LENGTH,
            .sample_rate = 16000,
            .caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
            .mode = AEC_MODE_SR_LOW_COST,
            .nlp_level = AEC_NLP_LEVEL_NORMAL,
        };
        aec_handle_t *handle = aec_create_from_config(&config);
        ESP_RETURN_ON_FALSE(handle != NULL, ESP_ERR_NO_MEM, "audio", "Stereo AEC initialization failed");
        const int samples = aec_get_chunksize(handle);
        if (samples <= 0 || samples > FULLOCH_AEC_CAPTURE_MAX_SAMPLES) {
            aec_destroy(handle);
            return ESP_FAIL;
        }
        int16_t *capture = heap_caps_aligned_alloc(16, (size_t)samples * FULLOCH_AEC_CAPTURE_CHANNELS * sizeof(*capture),
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        int16_t *microphones = heap_caps_aligned_alloc(16, (size_t)samples * 2 * sizeof(*microphones),
                                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        int16_t *reference = heap_caps_aligned_alloc(16, (size_t)samples * sizeof(*reference),
                                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        int16_t *output = heap_caps_aligned_alloc(16, (size_t)samples * 2 * sizeof(*output),
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (capture == NULL || microphones == NULL || reference == NULL || output == NULL) {
            free(capture);
            free(microphones);
            free(reference);
            free(output);
            aec_destroy(handle);
            return ESP_ERR_NO_MEM;
        }
        direct_aec = handle;
        direct_aec_samples = samples;
        direct_aec_capture = capture;
        direct_aec_microphones = microphones;
        direct_aec_reference = reference;
        direct_aec_output = output;
        aec_enabled = true;
        ESP_LOGI("audio", "Stereo direct AEC enabled: chunk=%d M1,M2 with synchronous R", samples);
        return ESP_OK;
    }
    afe_config_t *config = afe_config_init(AFE_INPUT_FORMAT, NULL, AFE_TYPE_VC, AFE_MODE_LOW_COST);
    ESP_RETURN_ON_FALSE(config != NULL, ESP_ERR_NO_MEM, "audio", "AFE configuration failed");
    config->aec_init = true;
    config->aec_mode = AEC_MODE_VOIP_LOW_COST;
    config->aec_filter_length = AFE_FILTER_LENGTH;
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
    if (afe_handle != NULL && afe_data != NULL) {
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
    ESP_LOGI("audio",
             "Runtime diagnostics: queues uplink=%u downlink=%u free=%u heap_internal=%u/%u psram=%u/%u "
             "stack_free_bytes capture=%u afe_fetch=%u uplink=%u downlink=%u",
             (unsigned int)uxQueueMessagesWaiting(uplink_queue), (unsigned int)uxQueueMessagesWaiting(downlink_queue),
             (unsigned int)uxQueueMessagesWaiting(downlink_free_queue),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
             (unsigned int)(uxTaskGetStackHighWaterMark(capture_task_handle) * sizeof(StackType_t)),
             (unsigned int)(uxTaskGetStackHighWaterMark(afe_fetch_task_handle) * sizeof(StackType_t)),
             (unsigned int)(uxTaskGetStackHighWaterMark(uplink_task_handle) * sizeof(StackType_t)),
             (unsigned int)(uxTaskGetStackHighWaterMark(downlink_task_handle) * sizeof(StackType_t)));
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
