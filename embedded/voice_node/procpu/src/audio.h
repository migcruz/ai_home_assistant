#pragma once

#include <stdint.h>
#include "audio_shared.h"

/*
 * Shared PSRAM audio buffer — written by appcpu, read by procpu.
 * Both cores access it via direct pointer (data cache, no DMA).
 *
 * The buffer is allocated in procpu's .ext_ram.bss (linker-managed PSRAM).
 * appcpu receives the runtime address via IPM_ID_BUFADDR at boot.
 * See embedded/common/audio_shared.h for the buffer layout (header + PCM).
 */
extern uint8_t audio_psram_buf[];
extern const uint8_t * const audio_buf;

/* Log the first 8 PCM samples and an RMS² sanity value from PSRAM. */
void log_audio_samples(uint32_t byte_count);
