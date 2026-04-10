#pragma once

#include <stdint.h>
#include "audio_shared.h"

/*
 * Shared PSRAM audio record buffer — written by appcpu, read by procpu.
 * Both cores access it via direct pointer (data cache, no DMA).
 *
 * The buffer is allocated in procpu's .ext_ram.bss (linker-managed PSRAM).
 * appcpu receives the runtime address via IPM_ID_BUFADDR at boot.
 * See embedded/common/audio_shared.h for the buffer layout (header + PCM).
 */
extern uint8_t audio_psram_buf[];
extern const uint8_t * const audio_buf;

/*
 * Shared PSRAM playback buffer — written by procpu (WebSocket RX),
 * read by appcpu (I2S TX via MAX98357A).
 *
 * procpu writes a struct play_shared header + WAV bytes, flushes the cache,
 * then sends IPM_ID_PLAY to appcpu.  appcpu receives the runtime address
 * at boot via IPM_ID_PLAYBUFADDR.
 * See embedded/common/audio_shared.h for the struct play_shared layout.
 *
 * play_buf is a typed alias over play_psram_buf, signalling that procpu owns
 * writes to this buffer (contrast: audio_buf is const — procpu only reads it).
 */
extern uint8_t play_psram_buf[];
extern struct play_shared * const play_buf;

/* Log the first 8 PCM samples and an RMS² sanity value from PSRAM. */
void log_audio_samples(uint32_t byte_count);
