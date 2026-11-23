# esp-idf-audio-mixer

[![ESP-IDF](https://img.shields.io/badge/ESP--IDF-v5.0%2B%20%7C%20v6.0%2B-blue.svg)](https://github.com/espressif/esp-idf)
[![Target](https://img.shields.io/badge/Target-ESP32%20%7C%20S3%20%7C%20P4-orange.svg)](https://www.espressif.com/)
[![License](https://img.shields.io/badge/License-Non--Commercial%20%2F%20Hobbyist-green.svg)](#license)

Universal, priority-driven real-time multi-channel PCM audio mixer for ESP-IDF. Optimized for embedded systems (ESP32, ESP32-S3, ESP32-P4) requiring concurrent playback of voice synthesis (TTS), web radio, system alerts, chimes, and sound effects.

---

## Highlights

- **Multi-Channel Real-Time Mixing:** Up to 16 concurrent channels (default: 8, configurable via `Kconfig`).
- **Priority Hierarchy & Intelligent Auto-Ducking:**
  - `CRITICAL` (Urgent alarms, system alerts)
  - `HIGH` (TTS voice prompts, voice assistant dialog)
  - `NORMAL` (Sound effects, audio player, notifications)
  - `BACKGROUND` (Ambient music, internet radio streams)
  - When an active channel with priority $\ge$ `HIGH` produces audio, background channels marked as `duckable` automatically ramp down smoothly (e.g. to 15% volume) and restore to full volume once speech ends.
- **Click-Free Gain Ramping (Anti-Click):** Dynamic linear per-sample fixed-point gain ramping for both ducking and channel fade-in / fade-out eliminates clicks, pops, and discontinuities.
- **Independent Ringbuffers:** Each channel owns an isolated FreeRTOS ringbuffer, allowing asynchronous audio producers (HTTP tasks, TTS engines, chime generators) to write without blocking each other.
- **Hardware-Agnostic Audio Sink:** Mixed 16-bit PCM frames are delivered via a lightweight callback (`audio_mixer_output_cb_t`) directly into I2S DMA, onboard codecs (e.g. ES8311), Audio HALs, or network streamers.
- **Volume & Limiter Engine:** Per-channel volume (0–100%), global master volume (0–100%), 32-bit sample accumulation with saturation clamping to prevent clipping distortion.

---

## Architecture

```
┌────────────────────────────────────────────────────────────────────────┐
│                          AUDIO PRODUCERS                               │
│                                                                        │
│   TTS Engine         System Chimes       Internet Radio      SFX / WAV │
│   [HIGH Priority]    [CRIT Priority]     [BACKGROUND/Duck]   [NORMAL]  │
└─────────┬───────────────────┬───────────────────┬────────────────┬─────┘
          │                   │                   │                │
          ▼                   ▼                   ▼                ▼
    ┌───────────┐       ┌───────────┐       ┌───────────┐    ┌───────────┐
    │ Ringbuf 0 │       │ Ringbuf 1 │       │ Ringbuf 2 │    │ Ringbuf 3 │
    └─────┬─────┘       └─────┬─────┘       └─────┬─────┘    └─────┬─────┘
          │                   │                   │                │
          └───────────────────┼─────────┬─────────┘                │
                              │         │                          │
                              ▼         ▼                          ▼
┌────────────────────────────────────────────────────────────────────────┐
│                        AUDIO MIXER WORKER TASK                         │
│                                                                        │
│  • Priority Evaluation & Automatic Ducking Trigger                     │
│  • Per-Sample Fixed-Point Gain Ramping (Anti-Click Smoothing)          │
│  • 32-Bit Sample Accumulator & Master Volume Scaling                   │
│  • Saturation Limiter / Clamping to [-32768, 32767]                    │
└───────────────────────────────────┬────────────────────────────────────┘
                                    │ Mixed 16-bit PCM Frame
                                    ▼
┌────────────────────────────────────────────────────────────────────────┐
│                    HARDWARE SINK (Output Callback)                     │
│                    audio_hal_write_pcm() / I2S DMA                     │
└────────────────────────────────────────────────────────────────────────┘
```

---

## Installation

Add this component to your ESP-IDF project manifest (`main/idf_component.yml`):

### Via Git Repository

```yaml
dependencies:
  esp-idf-audio-mixer:
    git: "https://github.com/Strg-Alt-Entf-0x00/esp-idf-audio-mixer.git"
```

Then run:
```bash
idf.py reconfigure
```

---

## Quick Start

### 1. Initialize the Mixer and Output Sink

```c
#include "audio_mixer.h"
#include "esp_log.h"

// Hardware sink callback (e.g. I2S or Audio HAL)
static esp_err_t audio_sink_callback(const int16_t *samples, size_t num_samples, void *ctx)
{
    // Write mixed PCM samples to your audio driver / I2S DMA
    return audio_hal_write_pcm(samples, num_samples);
}

void app_main(void)
{
    audio_mixer_config_t mixer_cfg = {
        .sample_rate      = 16000,
        .master_volume    = 100,
        .output_cb        = audio_sink_callback,
        .output_user_ctx  = NULL,
        .frame_samples    = 160,       // 10 ms frame at 16 kHz
        .task_priority    = 22,        // Dedicated real-time priority
        .task_core_id     = 1,         // Pin to Core 1
        .task_stack_size  = 4096,
        .duck_target_pct  = 15,        // Duck background to 15% during voice
        .duck_ramp_ms     = 50,        // 50 ms smooth transition for ducking
        .fade_ramp_ms     = 20,        // 20 ms anti-click fade in/out
    };

    ESP_ERROR_CHECK(audio_mixer_init(&mixer_cfg));
}
```

### 2. Register Channels & Push Audio

```c
// Open High-Priority TTS Channel (Not duckable, triggers ducking of others)
audio_mixer_channel_config_t tts_cfg = {
    .name                 = "tts",
    .priority             = AUDIO_MIX_PRIO_HIGH,
    .duckable             = false,
    .initial_volume       = 100,
    .ringbuffer_samples   = 4096,
};
audio_mixer_channel_t ch_tts = audio_mixer_open_channel(&tts_cfg);

// Open Background Radio Channel (Duckable)
audio_mixer_channel_config_t radio_cfg = {
    .name                 = "radio",
    .priority             = AUDIO_MIX_PRIO_BACKGROUND,
    .duckable             = true,
    .initial_volume       = 80,
    .ringbuffer_samples   = 8192,
};
audio_mixer_channel_t ch_radio = audio_mixer_open_channel(&radio_cfg);

// Write audio from your producer tasks:
audio_mixer_write(ch_tts, pcm_samples, sample_count, pdMS_TO_TICKS(100));
```

### 3. Dynamic Controls

```c
// Change volume on the fly (0 - 100)
audio_mixer_set_channel_volume(ch_radio, 65);
audio_mixer_set_master_volume(90);

// Mute / Unmute
audio_mixer_mute_channel(ch_radio, true);

// Flush buffer (e.g. when cancelling speech playback)
audio_mixer_flush_channel(ch_tts);

// Query channel status
audio_mixer_channel_status_t status;
if (audio_mixer_get_channel_status(ch_radio, &status) == ESP_OK) {
    ESP_LOGI("TAG", "Queued: %u samples, Ducked: %s", 
             (unsigned)status.samples_queued, status.is_ducked ? "YES" : "NO");
}
```

---

## Configuration Reference (Kconfig)

Configure defaults via `idf.py menuconfig` -> `Component config` -> `Audio Mixer Configuration`:

| Config Option | Default | Range | Description |
|---|---|---|---|
| `CONFIG_AUDIO_MIXER_MAX_CHANNELS` | `8` | 2 – 16 | Maximum concurrent channel slots |
| `CONFIG_AUDIO_MIXER_FRAME_SAMPLES` | `160` | 32 – 1024 | Processed samples per mixer cycle (160 = 10 ms at 16 kHz) |
| `CONFIG_AUDIO_MIXER_DEFAULT_RINGBUF_SAMPLES` | `4096` | 512 – 32768 | Default ringbuffer capacity per channel |
| `CONFIG_AUDIO_MIXER_TASK_PRIORITY` | `22` | 1 – 25 | FreeRTOS priority of mixer worker task |
| `CONFIG_AUDIO_MIXER_TASK_CORE_ID` | `1` | 0 – 1 | CPU core affinity for mixer task |
| `CONFIG_AUDIO_MIXER_TASK_STACK_SIZE` | `4096` | 2048 – 16384 | Task stack size in bytes |

---

## API Summary

| Function | Description |
|---|---|
| `audio_mixer_init(config)` | Initializes mixer and starts dedicated worker task. |
| `audio_mixer_deinit()` | Stops worker task, closes channels, and frees memory. |
| `audio_mixer_open_channel(cfg)` | Allocates ringbuffer and registers an audio channel. |
| `audio_mixer_close_channel(ch)` | Unregisters channel and frees its ringbuffer. |
| `audio_mixer_write(ch, samples, count, timeout)` | Writes 16-bit mono PCM into channel's ringbuffer. |
| `audio_mixer_set_channel_volume(ch, vol)` | Sets per-channel volume level (0–100%). |
| `audio_mixer_get_channel_volume(ch)` | Queries current channel volume. |
| `audio_mixer_mute_channel(ch, mute)` | Mutes or unmutes a specific channel. |
| `audio_mixer_set_master_volume(vol)` | Adjusts global output volume (0–100%). |
| `audio_mixer_get_master_volume()` | Queries current global output volume. |
| `audio_mixer_flush_channel(ch)` | Discards all unread PCM samples in channel buffer. |
| `audio_mixer_get_channel_free_samples(ch)` | Returns available write capacity in samples. |
| `audio_mixer_get_channel_status(ch, status)` | Returns active, ducking, buffer, and volume stats. |

---

## License

This project is licensed under the [PolyForm Noncommercial License 1.0.0](LICENSE).
- **Free for noncommercial use**: Personal, educational, research, and hobbyist projects are completely free.
- **Commercial use**: Not permitted without prior authorization. For commercial inquiries, please contact the author via GitHub.
