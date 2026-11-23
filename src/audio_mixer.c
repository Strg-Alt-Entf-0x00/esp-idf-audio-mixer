#include "audio_mixer.h"

#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "audio_mixer";

#define FP_SHIFT 16
#define FP_ONE   (1 << FP_SHIFT)

typedef struct {
    int16_t *buffer;
    size_t capacity;
    size_t head;
    size_t tail;
    size_t count;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t space_sem;
} channel_ringbuf_t;

typedef struct {
    bool in_use;
    char name[32];
    audio_mix_priority_t priority;
    bool duckable;
    bool active;
    bool muted;
    uint8_t user_volume;         // 0 - 100
    int32_t current_gain_fp;     // 16.16 fixed point (0 .. 1.0)
    int32_t target_gain_fp;      // 16.16 fixed point
    int32_t gain_step_fp;        // Step per sample during ramping
    uint64_t total_samples_written;
    channel_ringbuf_t ringbuf;
} mixer_channel_internal_t;

typedef struct {
    audio_mixer_config_t cfg;
    bool initialized;
    bool running;
    TaskHandle_t task_handle;
    SemaphoreHandle_t global_lock;
    mixer_channel_internal_t channels[AUDIO_MIXER_MAX_CHANNELS];
    int32_t *accum_buffer;
    int16_t *frame_buffer;
    int16_t *chan_read_buffer;
} audio_mixer_mgr_t;

static audio_mixer_mgr_t s_mixer = {0};

/* --- Ringbuffer Helpers --- */

static bool ringbuf_init(channel_ringbuf_t *rb, size_t capacity)
{
    rb->buffer = (int16_t *)malloc(capacity * sizeof(int16_t));
    if (!rb->buffer) {
        return false;
    }
    rb->capacity = capacity;
    rb->head = 0;
    rb->tail = 0;
    rb->count = 0;
    rb->lock = xSemaphoreCreateMutex();
    rb->space_sem = xSemaphoreCreateBinary();
    if (!rb->lock || !rb->space_sem) {
        if (rb->lock) vSemaphoreDelete(rb->lock);
        if (rb->space_sem) vSemaphoreDelete(rb->space_sem);
        free(rb->buffer);
        rb->buffer = NULL;
        return false;
    }
    xSemaphoreGive(rb->space_sem);
    return true;
}

static void ringbuf_deinit(channel_ringbuf_t *rb)
{
    if (rb->lock) {
        vSemaphoreDelete(rb->lock);
        rb->lock = NULL;
    }
    if (rb->space_sem) {
        vSemaphoreDelete(rb->space_sem);
        rb->space_sem = NULL;
    }
    if (rb->buffer) {
        free(rb->buffer);
        rb->buffer = NULL;
    }
    rb->capacity = 0;
    rb->count = 0;
}

static size_t ringbuf_write(channel_ringbuf_t *rb, const int16_t *samples, size_t num_samples)
{
    size_t written = 0;
    while (written < num_samples && rb->count < rb->capacity) {
        rb->buffer[rb->head] = samples[written++];
        rb->head = (rb->head + 1) % rb->capacity;
        rb->count++;
    }
    return written;
}

static size_t ringbuf_read(channel_ringbuf_t *rb, int16_t *dest, size_t max_samples)
{
    size_t read_count = 0;
    while (read_count < max_samples && rb->count > 0) {
        dest[read_count++] = rb->buffer[rb->tail];
        rb->tail = (rb->tail + 1) % rb->capacity;
        rb->count--;
    }
    return read_count;
}

/* --- Fixed-Point Math Helpers --- */

static inline int32_t float_to_fp(float v)
{
    return (int32_t)(v * (float)FP_ONE + 0.5f);
}

/* --- Mixer Task --- */

static void audio_mixer_worker_task(void *pvParameters)
{
    (void)pvParameters;
    const uint32_t frame_samples = s_mixer.cfg.frame_samples;
    const uint32_t sample_rate = s_mixer.cfg.sample_rate;
    const TickType_t frame_ticks = pdMS_TO_TICKS((frame_samples * 1000) / sample_rate);
    TickType_t last_wake_time = xTaskGetTickCount();

    ESP_LOGI(TAG, "Audio mixer task started (frame_size=%lu, rate=%lu Hz, ticks=%lu)",
             frame_samples, sample_rate, frame_ticks > 0 ? frame_ticks : 1);

    while (s_mixer.running) {
        // Step 1: Scan active channels to detect if high-priority audio is playing
        bool high_prio_active = false;

        xSemaphoreTake(s_mixer.global_lock, portMAX_DELAY);

        for (int i = 0; i < AUDIO_MIXER_MAX_CHANNELS; ++i) {
            mixer_channel_internal_t *ch = &s_mixer.channels[i];
            if (ch->in_use && ch->active && !ch->muted) {
                if (ch->priority >= AUDIO_MIX_PRIO_HIGH) {
                    if (ch->ringbuf.count > 0) {
                        high_prio_active = true;
                    }
                }
            }
        }

        // Step 2: Determine target gain for each channel (ducking & active state)
        const int32_t duck_target_fp = (int32_t)(((int64_t)s_mixer.cfg.duck_target_pct * FP_ONE) / 100);
        const uint32_t ramp_samples_duck = (s_mixer.cfg.duck_ramp_ms * sample_rate) / 1000;
        const uint32_t ramp_samples_fade = (s_mixer.cfg.fade_ramp_ms * sample_rate) / 1000;

        for (int i = 0; i < AUDIO_MIXER_MAX_CHANNELS; ++i) {
            mixer_channel_internal_t *ch = &s_mixer.channels[i];
            if (!ch->in_use) {
                continue;
            }

            int32_t desired_gain_fp = 0;
            if (ch->active && !ch->muted && ch->ringbuf.count > 0) {
                int32_t vol_gain_fp = (int32_t)(((int64_t)ch->user_volume * FP_ONE) / 100);
                if (ch->duckable && high_prio_active) {
                    desired_gain_fp = (int32_t)(((int64_t)vol_gain_fp * duck_target_fp) >> FP_SHIFT);
                } else {
                    desired_gain_fp = vol_gain_fp;
                }
            } else {
                desired_gain_fp = 0;
            }

            ch->target_gain_fp = desired_gain_fp;

            // Calculate per-sample step
            uint32_t ramp_samples = ch->duckable ? ramp_samples_duck : ramp_samples_fade;
            if (ramp_samples == 0) ramp_samples = 1;
            ch->gain_step_fp = (ch->target_gain_fp - ch->current_gain_fp) / (int32_t)ramp_samples;
            if (ch->gain_step_fp == 0 && ch->current_gain_fp != ch->target_gain_fp) {
                ch->gain_step_fp = (ch->target_gain_fp > ch->current_gain_fp) ? 1 : -1;
            }
        }

        // Step 3: Clear 32-bit mixing accumulator
        memset(s_mixer.accum_buffer, 0, frame_samples * sizeof(int32_t));

        // Step 4: Mix each channel
        bool any_audio_rendered = false;

        for (int i = 0; i < AUDIO_MIXER_MAX_CHANNELS; ++i) {
            mixer_channel_internal_t *ch = &s_mixer.channels[i];
            if (!ch->in_use) {
                continue;
            }

            // Read available samples from ringbuffer
            size_t samples_read = 0;
            xSemaphoreTake(ch->ringbuf.lock, portMAX_DELAY);
            samples_read = ringbuf_read(&ch->ringbuf, s_mixer.chan_read_buffer, frame_samples);
            xSemaphoreGive(ch->ringbuf.lock);

            if (samples_read > 0) {
                xSemaphoreGive(ch->ringbuf.space_sem);
            }

            // If channel is running dry or ramping down, pad remainder with 0
            if (samples_read < frame_samples) {
                memset(&s_mixer.chan_read_buffer[samples_read], 0, (frame_samples - samples_read) * sizeof(int16_t));
            }

            if (samples_read > 0 || ch->current_gain_fp > 0) {
                any_audio_rendered = true;
                int32_t g = ch->current_gain_fp;
                const int32_t target_g = ch->target_gain_fp;
                const int32_t step = ch->gain_step_fp;

                for (uint32_t s = 0; s < frame_samples; ++s) {
                    if (step > 0) {
                        g += step;
                        if (g > target_g) g = target_g;
                    } else if (step < 0) {
                        g += step;
                        if (g < target_g) g = target_g;
                    }

                    int32_t sample = s_mixer.chan_read_buffer[s];
                    int32_t scaled = (int32_t)(((int64_t)sample * g) >> FP_SHIFT);
                    s_mixer.accum_buffer[s] += scaled;
                }
                ch->current_gain_fp = g;
            }
        }

        const uint8_t master_vol = s_mixer.cfg.master_volume;
        audio_mixer_output_cb_t out_cb = s_mixer.cfg.output_cb;
        void *out_ctx = s_mixer.cfg.output_user_ctx;

        xSemaphoreGive(s_mixer.global_lock);

        // Step 5: Master volume scaling, clamping, and conversion to int16
        for (uint32_t s = 0; s < frame_samples; ++s) {
            int32_t sample = (s_mixer.accum_buffer[s] * master_vol) / 100;
            if (sample > 32767) {
                sample = 32767;
            } else if (sample < -32768) {
                sample = -32768;
            }
            s_mixer.frame_buffer[s] = (int16_t)sample;
        }

        // Step 6: Output sink
        if (out_cb) {
            out_cb(s_mixer.frame_buffer, frame_samples, out_ctx);
        } else {
            // If no output callback provided, sleep for frame duration
            vTaskDelayUntil(&last_wake_time, frame_ticks > 0 ? frame_ticks : 1);
        }
    }

    ESP_LOGI(TAG, "Audio mixer worker task terminated");
    vTaskDelete(NULL);
}

/* --- Public API Implementation --- */

esp_err_t audio_mixer_init(const audio_mixer_config_t *config)
{
    if (s_mixer.initialized) {
        ESP_LOGW(TAG, "Mixer already initialized");
        return ESP_ERR_INVALID_STATE;
    }

    memset(&s_mixer, 0, sizeof(s_mixer));

    if (config) {
        s_mixer.cfg = *config;
    } else {
        s_mixer.cfg.sample_rate = 16000;
        s_mixer.cfg.master_volume = 100;
    }

    if (s_mixer.cfg.sample_rate == 0) s_mixer.cfg.sample_rate = 16000;
    if (s_mixer.cfg.frame_samples == 0) s_mixer.cfg.frame_samples = AUDIO_MIXER_FRAME_SAMPLES;
    if (s_mixer.cfg.task_priority == 0) s_mixer.cfg.task_priority = 22;
    if (s_mixer.cfg.task_stack_size == 0) s_mixer.cfg.task_stack_size = 4096;
    if (s_mixer.cfg.duck_target_pct == 0) s_mixer.cfg.duck_target_pct = 15;
    if (s_mixer.cfg.duck_ramp_ms == 0) s_mixer.cfg.duck_ramp_ms = 50;
    if (s_mixer.cfg.fade_ramp_ms == 0) s_mixer.cfg.fade_ramp_ms = 20;

    s_mixer.global_lock = xSemaphoreCreateMutex();
    if (!s_mixer.global_lock) {
        return ESP_ERR_NO_MEM;
    }

    s_mixer.accum_buffer = (int32_t *)calloc(s_mixer.cfg.frame_samples, sizeof(int32_t));
    s_mixer.frame_buffer = (int16_t *)calloc(s_mixer.cfg.frame_samples, sizeof(int16_t));
    s_mixer.chan_read_buffer = (int16_t *)calloc(s_mixer.cfg.frame_samples, sizeof(int16_t));

    if (!s_mixer.accum_buffer || !s_mixer.frame_buffer || !s_mixer.chan_read_buffer) {
        audio_mixer_deinit();
        return ESP_ERR_NO_MEM;
    }

    s_mixer.running = true;
    BaseType_t ret = xTaskCreatePinnedToCore(
        audio_mixer_worker_task,
        "audio_mixer_task",
        s_mixer.cfg.task_stack_size,
        NULL,
        s_mixer.cfg.task_priority,
        &s_mixer.task_handle,
        s_mixer.cfg.task_core_id
    );

    if (ret != pdPASS) {
        audio_mixer_deinit();
        return ESP_FAIL;
    }

    s_mixer.initialized = true;
    ESP_LOGI(TAG, "Audio mixer initialized successfully (sample_rate=%lu, channels=%d)",
             s_mixer.cfg.sample_rate, AUDIO_MIXER_MAX_CHANNELS);
    return ESP_OK;
}

void audio_mixer_deinit(void)
{
    if (!s_mixer.initialized && !s_mixer.running) {
        return;
    }

    s_mixer.running = false;
    if (s_mixer.task_handle) {
        // Wait for task termination
        vTaskDelay(pdMS_TO_TICKS(50));
        s_mixer.task_handle = NULL;
    }

    if (s_mixer.global_lock) {
        xSemaphoreTake(s_mixer.global_lock, portMAX_DELAY);
    }

    for (int i = 0; i < AUDIO_MIXER_MAX_CHANNELS; ++i) {
        if (s_mixer.channels[i].in_use) {
            ringbuf_deinit(&s_mixer.channels[i].ringbuf);
            s_mixer.channels[i].in_use = false;
        }
    }

    if (s_mixer.accum_buffer) {
        free(s_mixer.accum_buffer);
        s_mixer.accum_buffer = NULL;
    }
    if (s_mixer.frame_buffer) {
        free(s_mixer.frame_buffer);
        s_mixer.frame_buffer = NULL;
    }
    if (s_mixer.chan_read_buffer) {
        free(s_mixer.chan_read_buffer);
        s_mixer.chan_read_buffer = NULL;
    }

    if (s_mixer.global_lock) {
        xSemaphoreGive(s_mixer.global_lock);
        vSemaphoreDelete(s_mixer.global_lock);
        s_mixer.global_lock = NULL;
    }

    s_mixer.initialized = false;
    ESP_LOGI(TAG, "Audio mixer deinitialized");
}

audio_mixer_channel_t audio_mixer_open_channel(const audio_mixer_channel_config_t *config)
{
    if (!s_mixer.initialized || !config) {
        return AUDIO_MIXER_CHANNEL_INVALID;
    }

    xSemaphoreTake(s_mixer.global_lock, portMAX_DELAY);

    audio_mixer_channel_t free_slot = AUDIO_MIXER_CHANNEL_INVALID;
    for (int i = 0; i < AUDIO_MIXER_MAX_CHANNELS; ++i) {
        if (!s_mixer.channels[i].in_use) {
            free_slot = i;
            break;
        }
    }

    if (free_slot == AUDIO_MIXER_CHANNEL_INVALID) {
        xSemaphoreGive(s_mixer.global_lock);
        ESP_LOGE(TAG, "Cannot open channel '%s': all %d slots occupied",
                 config->name ? config->name : "unnamed", AUDIO_MIXER_MAX_CHANNELS);
        return AUDIO_MIXER_CHANNEL_INVALID;
    }

    mixer_channel_internal_t *ch = &s_mixer.channels[free_slot];
    memset(ch, 0, sizeof(*ch));

    size_t ringbuf_cap = config->ringbuffer_samples > 0 ? config->ringbuffer_samples : AUDIO_MIXER_DEFAULT_RINGBUF_SAMPLES;
    if (!ringbuf_init(&ch->ringbuf, ringbuf_cap)) {
        xSemaphoreGive(s_mixer.global_lock);
        ESP_LOGE(TAG, "Failed to allocate ringbuffer for channel %d", free_slot);
        return AUDIO_MIXER_CHANNEL_INVALID;
    }

    strncpy(ch->name, config->name ? config->name : "channel", sizeof(ch->name) - 1);
    ch->priority = config->priority;
    ch->duckable = config->duckable;
    ch->user_volume = config->initial_volume > 100 ? 100 : config->initial_volume;
    ch->active = true;
    ch->muted = false;
    ch->current_gain_fp = 0;
    ch->target_gain_fp = (int32_t)(((int64_t)ch->user_volume * FP_ONE) / 100);
    ch->in_use = true;

    xSemaphoreGive(s_mixer.global_lock);

    ESP_LOGI(TAG, "Channel '%s' opened as id=%d (prio=%d, duckable=%d, vol=%d, buf=%lu)",
             ch->name, free_slot, ch->priority, ch->duckable, ch->user_volume, ringbuf_cap);

    return free_slot;
}

esp_err_t audio_mixer_close_channel(audio_mixer_channel_t ch_id)
{
    if (!s_mixer.initialized || ch_id < 0 || ch_id >= AUDIO_MIXER_MAX_CHANNELS) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mixer.global_lock, portMAX_DELAY);
    mixer_channel_internal_t *ch = &s_mixer.channels[ch_id];
    if (!ch->in_use) {
        xSemaphoreGive(s_mixer.global_lock);
        return ESP_ERR_INVALID_STATE;
    }

    ch->in_use = false;
    ch->active = false;
    ringbuf_deinit(&ch->ringbuf);
    xSemaphoreGive(s_mixer.global_lock);

    ESP_LOGI(TAG, "Channel id=%d closed", ch_id);
    return ESP_OK;
}

esp_err_t audio_mixer_write(audio_mixer_channel_t ch_id,
                            const int16_t *samples,
                            size_t num_samples,
                            TickType_t timeout_ticks)
{
    if (!s_mixer.initialized || ch_id < 0 || ch_id >= AUDIO_MIXER_MAX_CHANNELS || !samples || num_samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    mixer_channel_internal_t *ch = &s_mixer.channels[ch_id];
    if (!ch->in_use) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t written_total = 0;
    TickType_t start_tick = xTaskGetTickCount();

    while (written_total < num_samples && s_mixer.running) {
        xSemaphoreTake(ch->ringbuf.lock, portMAX_DELAY);
        size_t written = ringbuf_write(&ch->ringbuf, &samples[written_total], num_samples - written_total);
        xSemaphoreGive(ch->ringbuf.lock);

        written_total += written;
        ch->total_samples_written += written;

        if (written_total < num_samples) {
            TickType_t elapsed = xTaskGetTickCount() - start_tick;
            if (elapsed >= timeout_ticks) {
                return ESP_ERR_TIMEOUT;
            }
            TickType_t wait_ticks = timeout_ticks - elapsed;
            xSemaphoreTake(ch->ringbuf.space_sem, wait_ticks > 0 ? wait_ticks : 1);
        }
    }

    return ESP_OK;
}

esp_err_t audio_mixer_set_channel_volume(audio_mixer_channel_t ch_id, uint8_t vol)
{
    if (!s_mixer.initialized || ch_id < 0 || ch_id >= AUDIO_MIXER_MAX_CHANNELS) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mixer.global_lock, portMAX_DELAY);
    mixer_channel_internal_t *ch = &s_mixer.channels[ch_id];
    if (!ch->in_use) {
        xSemaphoreGive(s_mixer.global_lock);
        return ESP_ERR_INVALID_STATE;
    }
    ch->user_volume = (vol > 100) ? 100 : vol;
    xSemaphoreGive(s_mixer.global_lock);
    return ESP_OK;
}

uint8_t audio_mixer_get_channel_volume(audio_mixer_channel_t ch_id)
{
    if (!s_mixer.initialized || ch_id < 0 || ch_id >= AUDIO_MIXER_MAX_CHANNELS) {
        return 0;
    }
    return s_mixer.channels[ch_id].user_volume;
}

esp_err_t audio_mixer_mute_channel(audio_mixer_channel_t ch_id, bool mute)
{
    if (!s_mixer.initialized || ch_id < 0 || ch_id >= AUDIO_MIXER_MAX_CHANNELS) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mixer.global_lock, portMAX_DELAY);
    mixer_channel_internal_t *ch = &s_mixer.channels[ch_id];
    if (!ch->in_use) {
        xSemaphoreGive(s_mixer.global_lock);
        return ESP_ERR_INVALID_STATE;
    }
    ch->muted = mute;
    xSemaphoreGive(s_mixer.global_lock);
    return ESP_OK;
}

bool audio_mixer_is_channel_muted(audio_mixer_channel_t ch_id)
{
    if (!s_mixer.initialized || ch_id < 0 || ch_id >= AUDIO_MIXER_MAX_CHANNELS) {
        return false;
    }
    return s_mixer.channels[ch_id].muted;
}

esp_err_t audio_mixer_set_channel_active(audio_mixer_channel_t ch_id, bool active)
{
    if (!s_mixer.initialized || ch_id < 0 || ch_id >= AUDIO_MIXER_MAX_CHANNELS) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mixer.global_lock, portMAX_DELAY);
    mixer_channel_internal_t *ch = &s_mixer.channels[ch_id];
    if (!ch->in_use) {
        xSemaphoreGive(s_mixer.global_lock);
        return ESP_ERR_INVALID_STATE;
    }
    ch->active = active;
    xSemaphoreGive(s_mixer.global_lock);
    return ESP_OK;
}

bool audio_mixer_is_channel_active(audio_mixer_channel_t ch_id)
{
    if (!s_mixer.initialized || ch_id < 0 || ch_id >= AUDIO_MIXER_MAX_CHANNELS) {
        return false;
    }
    return s_mixer.channels[ch_id].active;
}

void audio_mixer_set_master_volume(uint8_t vol)
{
    if (!s_mixer.initialized) {
        return;
    }
    s_mixer.cfg.master_volume = (vol > 100) ? 100 : vol;
}

uint8_t audio_mixer_get_master_volume(void)
{
    if (!s_mixer.initialized) {
        return 0;
    }
    return s_mixer.cfg.master_volume;
}

void audio_mixer_flush_channel(audio_mixer_channel_t ch_id)
{
    if (!s_mixer.initialized || ch_id < 0 || ch_id >= AUDIO_MIXER_MAX_CHANNELS) {
        return;
    }

    mixer_channel_internal_t *ch = &s_mixer.channels[ch_id];
    if (!ch->in_use) {
        return;
    }

    xSemaphoreTake(ch->ringbuf.lock, portMAX_DELAY);
    ch->ringbuf.head = 0;
    ch->ringbuf.tail = 0;
    ch->ringbuf.count = 0;
    xSemaphoreGive(ch->ringbuf.lock);
    xSemaphoreGive(ch->ringbuf.space_sem);
}

size_t audio_mixer_get_channel_free_samples(audio_mixer_channel_t ch_id)
{
    if (!s_mixer.initialized || ch_id < 0 || ch_id >= AUDIO_MIXER_MAX_CHANNELS) {
        return 0;
    }

    mixer_channel_internal_t *ch = &s_mixer.channels[ch_id];
    if (!ch->in_use) {
        return 0;
    }

    xSemaphoreTake(ch->ringbuf.lock, portMAX_DELAY);
    size_t free_samples = ch->ringbuf.capacity - ch->ringbuf.count;
    xSemaphoreGive(ch->ringbuf.lock);
    return free_samples;
}

esp_err_t audio_mixer_get_channel_status(audio_mixer_channel_t ch_id, audio_mixer_channel_status_t *status)
{
    if (!s_mixer.initialized || ch_id < 0 || ch_id >= AUDIO_MIXER_MAX_CHANNELS || !status) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mixer.global_lock, portMAX_DELAY);
    mixer_channel_internal_t *ch = &s_mixer.channels[ch_id];
    status->in_use = ch->in_use;
    status->active = ch->active;
    status->muted = ch->muted;
    status->volume = ch->user_volume;
    status->effective_volume = (uint8_t)(((int64_t)ch->current_gain_fp * 100) >> FP_SHIFT);
    status->is_ducked = (status->effective_volume < status->volume && ch->duckable);
    status->priority = ch->priority;
    status->buffer_capacity = ch->ringbuf.capacity;
    status->samples_queued = ch->ringbuf.count;
    status->total_samples_written = ch->total_samples_written;
    xSemaphoreGive(s_mixer.global_lock);

    return ESP_OK;
}

