#pragma once

#include <stdint.h>

/*
 * Shared PSRAM audio buffer — written by appcpu, read by procpu.
 * Both cores access it via direct pointer (data cache, no DMA).
 */
#define AUDIO_PSRAM_BASE  0x3C000000UL
#define AUDIO_BUF_MAX     (512U * 1024U)

extern const uint8_t * const audio_buf;

/* Log the first 8 PCM samples and an RMS² sanity value from PSRAM. */
void log_audio_samples(uint32_t byte_count);
