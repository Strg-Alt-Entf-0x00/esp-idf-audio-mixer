#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CONFIG_AUDIO_MIXER_MAX_CHANNELS
#define AUDIO_MIXER_MAX_CHANNELS 8
#else
#define AUDIO_MIXER_MAX_CHANNELS CONFIG_AUDIO_MIXER_MAX_CHANNELS
#endif

#ifndef CONFIG_AUDIO_MIXER_FRAME_SAMPLES
#define AUDIO_MIXER_FRAME_SAMPLES 160
#else
#define AUDIO_MIXER_FRAME_SAMPLES CONFIG_AUDIO_MIXER_FRAME_SAMPLES
#endif

#ifndef CONFIG_AUDIO_MIXER_DEFAULT_RINGBUF_SAMPLES
#define AUDIO_MIXER_DEFAULT_RINGBUF_SAMPLES 4096
#else
#define AUDIO_MIXER_DEFAULT_RINGBUF_SAMPLES CONFIG_AUDIO_MIXER_DEFAULT_RINGBUF_SAMPLES
#endif

#define AUDIO_MIXER_CHANNEL_INVALID (-1)

/**
 * @brief Priority levels for audio mixing and ducking hierarchy.
 */
typedef enum {
    AUDIO_MIX_PRIO_BACKGROUND = 0,  /**< Ambient audio, background radio */
    AUDIO_MIX_PRIO_NORMAL     = 5,  /**< Music player, regular sounds */
    AUDIO_MIX_PRIO_HIGH       = 10, /**< TTS speech, voice interaction */
    AUDIO_MIX_PRIO_CRITICAL   = 15, /**< System alerts, alarms, urgent chime */
} audio_mix_priority_t;

/**
 * @brief Channel identifier handle.
 */
typedef int audio_mixer_channel_t;

/**
 * @brief PCM output callback. Called by mixer task to write mixed frame to sink (e.g. I2S/codec).
 *
 * @param samples Pointer to mono int16_t samples
 * @param num_samples Number of samples in frame
 * @param user_ctx User-supplied context pointer
 * @return ESP_OK on success, or error code
 */
typedef esp_err_t (*audio_mixer_output_cb_t)(const int16_t *samples, size_t num_samples, void *user_ctx);

/**
 * @brief Configuration parameters for the mixer instance.
 */
typedef struct {
    uint32_t sample_rate;               /**< Audio sample rate in Hz (e.g. 16000) */
    uint8_t  master_volume;             /**< Master volume level (0 - 100, default 100) */
    audio_mixer_output_cb_t output_cb;  /**< Sinks mixed PCM frames to audio hardware */
    void *output_user_ctx;              /**< Context passed to output_cb */
    uint32_t frame_samples;             /**< Frame size in samples (0 = default AUDIO_MIXER_FRAME_SAMPLES) */
    UBaseType_t task_priority;          /**< FreeRTOS task priority (0 = default 22) */
    BaseType_t task_core_id;            /**< CPU Core ID (0 or 1, default 1) */
    uint32_t task_stack_size;           /**< Task stack size (0 = default 4096) */
    uint32_t duck_target_pct;           /**< Volume percentage during ducking (0-100, default 15) */
    uint32_t duck_ramp_ms;              /**< Ducking ramp duration in ms (default 50) */
    uint32_t fade_ramp_ms;              /**< Anti-click soft fade-in/out duration in ms (default 20) */
} audio_mixer_config_t;

/**
 * @brief Configuration for registering an audio input channel.
 */
typedef struct {
    const char *name;                   /**< Human-readable identifier (e.g. "tts", "syssnd", "radio") */
    audio_mix_priority_t priority;      /**< Mixing priority level */
    bool duckable;                      /**< If true, ducks when priority >= HIGH is actively playing */
    uint8_t initial_volume;             /**< Initial channel volume (0 - 100, default 100) */
    size_t ringbuffer_samples;          /**< Channel buffer size in samples (0 = default 4096) */
} audio_mixer_channel_config_t;

/**
 * @brief Channel status snapshot.
 */
typedef struct {
    bool in_use;                        /**< Channel is opened */
    bool active;                        /**< Channel is actively supplying audio */
    bool muted;                         /**< Channel is muted */
    bool is_ducked;                     /**< Currently ducked */
    uint8_t volume;                     /**< User-set channel volume (0-100) */
    uint8_t effective_volume;           /**< Current multiplier after ducking & fade (0-100) */
    audio_mix_priority_t priority;      /**< Assigned priority */
    size_t samples_queued;              /**< Number of samples currently queued in buffer */
    size_t buffer_capacity;             /**< Total buffer capacity in samples */
    uint64_t total_samples_written;     /**< Lifetime sample count written to this channel */
} audio_mixer_channel_status_t;

#ifdef __cplusplus
}
#endif

