#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>
#include <stdbool.h>

/**
 * Initialize the audio processing pipeline (Opus encoder, buffers).
 * Must be called once from main before starting audio_task.
 * Returns 0 on success, -1 on Opus init failure.
 */
int audio_init(void);

/**
 * Audio processing task entry point (FreeRTOS).
 * Blocks on USB audio semaphore, processes PCM, encodes Opus,
 * accumulates 2 frames and sends BT report 0x39 (double-frame).
 */
void audio_task(void *arg);

/**
 * Set headset plug state (parsed from BT input report byte[56] bit0).
 * When plugged, speaker tag switches from 0x93 to 0x96.
 */
void audio_set_headset(bool plugged);

/**
 * Reset audio state (called on controller disconnect).
 */
void audio_reset(void);

/**
 * Reset speaker encoder state (called on USB speaker stream close/open).
 * Clears Opus encoder internal prediction and frame_slot to prevent
 * quality degradation across stream close/open cycles.
 */
void audio_reset_encoder(void);

/**
 * Feed a mic Opus frame from the controller (called from BT callback context).
 * @param opus_data  pointer to the Opus-encoded frame
 * @param len        available byte length (must be >= 71)
 */
void audio_mic_feed(const uint8_t *opus_data, uint16_t len);

/**
 * Set mic active state (called when USB host opens/closes mic interface).
 * Sends a 0x32 status report to the controller to start/stop mic streaming.
 */
void audio_set_mic_active(bool active);

/**
 * Check if mic streaming is active.
 */
bool audio_mic_active(void);

/**
 * Mic decode task entry point (FreeRTOS).
 * Blocks on mic_queue, decodes Opus frames, writes to USB mic ring buffer.
 * Must run at lower priority than audio_task to avoid blocking speaker path.
 */
void audio_mic_task(void *arg);

/* S36：运行时调编码参数（0xF6 命令 0x10）。kbps 8..160；bw 0 自动 / 1 NB / 2 MB / 3 WB / 4 SWB / 5 FB；
 * mode 0 = 补填充到 200 B（线上格式不变）/ 1 = 按真实长度发；complexity 0..10。ISR 可调，audio_task 应用。 */
void audio_set_tuning(uint16_t kbps, uint8_t bw, uint8_t mode, uint8_t complexity);

/* R3.28：按配置体里的 audio_kbps / audio_bw / audio_cx 应用编码参数
 * （开机读配置后一次；网页「写入接收器」改配置后再一次）。 */
void audio_apply_config(void);

/**
 * Check if the audio task was killed by the encode watchdog and respawn it.
 * Call periodically from bt_task or main loop. Returns true if respawned.
 */
bool audio_check_respawn(void);

#endif /* AUDIO_H */
