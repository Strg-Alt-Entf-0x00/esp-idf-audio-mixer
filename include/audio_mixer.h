#pragma once

#include "audio_mixer_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize the audio mixer and start the mixer task.
 *
 * @param config Pointer to configuration struct. If NULL, defaults are used with NULL output.
 * @return ESP_OK on success, or appropriate error code.
 */
esp_err_t audio_mixer_init(const audio_mixer_config_t *config);

/**
 * @brief Deinitialize audio mixer, stop mixer task, and free all channel buffers.
 */
void audio_mixer_deinit(void);

/**
 * @brief Register a new audio channel.
 *
 * @param config Channel configuration parameters.
 * @return Channel handle (>= 0), or AUDIO_MIXER_CHANNEL_INVALID (-1) on error/limit.
 */
audio_mixer_channel_t audio_mixer_open_channel(const audio_mixer_channel_config_t *config);

/**
 * @brief Close and unregister an audio channel, freeing its ringbuffer.
 *
 * @param ch Channel handle returned by audio_mixer_open_channel.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if channel invalid.
 */
esp_err_t audio_mixer_close_channel(audio_mixer_channel_t ch);

/**
 * @brief Write PCM samples (16-bit signed mono) into channel's ringbuffer.
 *
 * @param ch Channel handle.
 * @param samples Pointer to 16-bit PCM samples.
 * @param num_samples Number of samples to write.
 * @param timeout_ticks Maximum FreeRTOS ticks to wait for buffer space.
 * @return ESP_OK on success, ESP_ERR_TIMEOUT if timed out, ESP_ERR_INVALID_ARG if invalid.
 */
esp_err_t audio_mixer_write(audio_mixer_channel_t ch,
                            const int16_t *samples,
                            size_t num_samples,
                            TickType_t timeout_ticks);

/**
 * @brief Set channel volume level.
 *
 * @param ch Channel handle.
 * @param vol Volume level (0 to 100).
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if invalid.
 */
esp_err_t audio_mixer_set_channel_volume(audio_mixer_channel_t ch, uint8_t vol);

/**
 * @brief Get channel volume level.
 *
 * @param ch Channel handle.
 * @return Volume level (0 to 100), or 0 on error.
 */
uint8_t audio_mixer_get_channel_volume(audio_mixer_channel_t ch);

/**
 * @brief Mute or unmute a channel.
 *
 * @param ch Channel handle.
 * @param mute true to mute, false to unmute.
 * @return ESP_OK on success.
 */
esp_err_t audio_mixer_mute_channel(audio_mixer_channel_t ch, bool mute);

/**
 * @brief Query if a channel is muted.
 *
 * @param ch Channel handle.
 * @return true if muted, false otherwise.
 */
bool audio_mixer_is_channel_muted(audio_mixer_channel_t ch);

/**
 * @brief Explicitly set channel active flag.
 *
 * @param ch Channel handle.
 * @param active true to enable playback from channel, false to pause.
 * @return ESP_OK on success.
 */
esp_err_t audio_mixer_set_channel_active(audio_mixer_channel_t ch, bool active);

/**
 * @brief Query if channel is currently marked active.
 *
 * @param ch Channel handle.
 * @return true if active, false otherwise.
 */
bool audio_mixer_is_channel_active(audio_mixer_channel_t ch);

/**
 * @brief Set master volume level.
 *
 * @param vol Master volume level (0 to 100).
 */
void audio_mixer_set_master_volume(uint8_t vol);

/**
 * @brief Get master volume level.
 *
 * @return Master volume level (0 to 100).
 */
uint8_t audio_mixer_get_master_volume(void);

/**
 * @brief Flush and clear all queued samples in a channel's ringbuffer.
 *
 * @param ch Channel handle.
 */
void audio_mixer_flush_channel(audio_mixer_channel_t ch);

/**
 * @brief Get available free space (in samples) in channel buffer.
 *
 * @param ch Channel handle.
 * @return Number of free samples available to write.
 */
size_t audio_mixer_get_channel_free_samples(audio_mixer_channel_t ch);

/**
 * @brief Retrieve current status of a channel.
 *
 * @param ch Channel handle.
 * @param status Pointer to status struct to populate.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG on failure.
 */
esp_err_t audio_mixer_get_channel_status(audio_mixer_channel_t ch, audio_mixer_channel_status_t *status);

#ifdef __cplusplus
}
#endif

