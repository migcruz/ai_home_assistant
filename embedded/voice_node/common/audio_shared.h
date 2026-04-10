#pragma once

#include <stdint.h>

/*
 * Shared PSRAM audio buffer layout.
 *
 * appcpu writes a struct audio_shared header followed immediately by raw
 * mono PCM.  procpu reads it after receiving IPM_ID_DONE.
 *
 * Physical address is negotiated at boot via IPM_ID_BUFADDR — procpu sends
 * the runtime address of its linker-allocated audio_psram_buf[] to appcpu.
 * AUDIO_PSRAM_BASE in appcpu/src/pdm.h is a compile-time fallback only.
 *
 * Total buffer size: AUDIO_BUF_MAX bytes.
 * PCM capacity:      AUDIO_PCM_MAX bytes  (AUDIO_BUF_MAX minus header).
 * Max recording:     ~16 s at 16 kHz, 16-bit, mono.
 */

#define AUDIO_BUF_MAX     (512U * 1024U)
#define AUDIO_SAMPLE_RATE 16000U
#define AUDIO_BITS        16U
#define AUDIO_CHANNELS    1U

/* Written by appcpu into the first bytes of the shared buffer. */
#define AUDIO_SHARED_MAGIC 0xA0D10C5EU

struct audio_shared {
	uint32_t magic;        /* AUDIO_SHARED_MAGIC — sanity check         */
	uint32_t byte_count;   /* PCM bytes written after this header        */
	uint32_t sample_rate;  /* samples per second (AUDIO_SAMPLE_RATE)     */
	uint16_t channels;     /* channel count (AUDIO_CHANNELS)             */
	uint16_t bits;         /* bits per sample (AUDIO_BITS)               */
	uint8_t  pcm[];        /* PCM data follows immediately               */
};

/* Maximum PCM bytes that fit after the header. */
#define AUDIO_PCM_MAX  (AUDIO_BUF_MAX - (uint32_t)sizeof(struct audio_shared))

/* ── Playback shared buffer ────────────────────────────────────────────────── */

/*
 * Shared PSRAM playback buffer layout.
 *
 * procpu receives TTS WAV frames over WebSocket and writes them here.
 * It then flushes the cache and sends IPM_ID_PLAY to appcpu (signal only).
 * appcpu invalidates its cache, reads len, and plays wav[].
 *
 * Physical address is negotiated at boot via IPM_ID_PLAYBUFADDR — procpu
 * sends the runtime address of its linker-allocated play_psram_buf[] to
 * appcpu.
 *
 * Total buffer size: PLAY_BUF_MAX bytes.
 * Covers sentences up to ~4.5 s at 22050 Hz, 16-bit, mono (with headroom).
 */
#define PLAY_BUF_MAX       (200U * 1024U)
#define PLAY_SHARED_MAGIC  0x504C4159U  /* "PLAY" */

struct play_shared {
	uint32_t magic;  /* PLAY_SHARED_MAGIC — sanity check                */
	uint32_t len;    /* byte length of the WAV in wav[] (set by procpu) */
	uint8_t  wav[];  /* raw WAV bytes (header + PCM) follow immediately */
};

#define PLAY_DATA_MAX  (PLAY_BUF_MAX - (uint32_t)sizeof(struct play_shared))
