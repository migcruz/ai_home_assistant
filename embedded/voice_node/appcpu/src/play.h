#pragma once

#include <stdint.h>
#include <zephyr/drivers/ipm.h>

/*
 * Initialize I2S1 TX for speaker playback (MAX98357A).
 * ipm is used for diagnostic logging during development.
 * Must be called once before play_from_shared().
 */
int play_init(const struct device *ipm);

/*
 * Play a WAV from a struct play_shared in PSRAM at the given address.
 * Invalidates the cache, validates the magic, and plays wav[0..len].
 * Blocks until all audio has been clocked out (DRAIN).
 */
int play_from_shared(uint32_t psram_addr);

/*
 * Loopback test: play back a recording from struct audio_shared in PSRAM.
 * Reads sample_rate + raw PCM directly — no WAV header needed.
 * play_buf_addr is unused (kept for API symmetry with main.c; may be removed).
 * Blocks until all audio has been clocked out.
 */
int play_audio_shared(uint32_t rec_addr, uint32_t play_buf_addr);

/*
 * Speaker path diagnostic: play a generated tone on I2S1.
 * Useful to isolate amplifier/I2S TX issues from microphone capture data.
 */
int play_test_tone(uint32_t duration_ms, uint32_t freq_hz);
