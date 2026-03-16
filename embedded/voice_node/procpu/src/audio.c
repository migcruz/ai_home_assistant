#include "audio.h"

#include <zephyr/kernel.h>
#include <zephyr/cache.h>
#include <zephyr/logging/log.h>
#include <stdint.h>
#include <math.h>

LOG_MODULE_DECLARE(procpu, LOG_LEVEL_DBG);

/*
 * Shared audio buffer in PSRAM, allocated via the linker's .ext_ram.bss section.
 * appcpu writes PCM here during recording; procpu reads after IPM "done" signal.
 *
 * NOTE: 0x3C000000 is NOT PSRAM — it's flash-mapped rodata.  Actual PSRAM starts
 * after the .ext_ram.dummy section (currently ~0x3C0B0000).  This buffer is
 * placed by the linker at the correct PSRAM address.  After building, check the
 * map file for the actual address and update AUDIO_PSRAM_BASE in both
 * procpu/src/audio.h and appcpu/src/pdm.h to match.
 */
uint8_t audio_psram_buf[AUDIO_BUF_MAX]
	__attribute__((section(".ext_ram.bss"))) __attribute__((aligned(64)));

const uint8_t * const audio_buf = audio_psram_buf;

void log_audio_samples(uint32_t byte_count)
{
	if (byte_count < 16) {
		LOG_WRN("[C0] too few bytes to inspect (%u)", byte_count);
		return;
	}

	/* Invalidate procpu's cache for this region before reading,
	 * ensuring we see the data appcpu flushed from its cache. */
	sys_cache_data_invd_range((void *)audio_buf, byte_count);

	/* PCM is 16-bit signed little-endian, stereo-interleaved.
	 * Even indices = left channel (mic), odd = right (zeros). */
	const int16_t *samples = (const int16_t *)audio_buf;
	uint32_t       n       = (byte_count / 2 < 8) ? byte_count / 2 : 8;

	LOG_INF("[C0] first %u PCM samples from PSRAM:", n);
	for (uint32_t i = 0; i < n; i++) {
		LOG_INF("  [%u] %d", i, samples[i]);
	}

	/* RMS² over first 256 samples (includes startup transients) */
	uint32_t count  = (byte_count / 2 < 256) ? byte_count / 2 : 256;
	int64_t  sum_sq = 0;

	for (uint32_t i = 0; i < count; i++) {
		int32_t s = samples[i];
		sum_sq += s * s;
	}
	LOG_INF("[C0] RMS² first  256 samples: %u", (uint32_t)(sum_sq / count));

	/* RMS² over 256 samples from the MIDDLE of the recording.
	 * If this is much lower than the first-256 RMS², the high RMS above
	 * is just startup transients — not ongoing noise. */
	uint32_t total  = byte_count / 2;               /* int16_t count   */
	uint32_t mid    = (total / 2 > 128) ? total / 2 - 128 : 0;
	uint32_t m_count = (total - mid < 256) ? total - mid : 256;
	int64_t  m_sq   = 0;

	/* Show 4 mid-point samples */
	LOG_INF("[C0] mid-point samples (idx %u+):", mid);
	for (uint32_t i = mid; i < mid + 4 && i < total; i++) {
		LOG_INF("  [%u] %d", i, samples[i]);
	}

	/* Mid-point RMS² for combined stereo */
	for (uint32_t i = mid; i < mid + m_count; i++) {
		int32_t s = samples[i];
		m_sq += s * s;
	}
	LOG_INF("[C0] RMS² middle 256 samples (combined): %u",
		(uint32_t)(m_sq / m_count));

	/* Separate L/R channel RMS² over the same mid-point window.
	 * Even stereo indices = L (mic data), odd = R (should be ~0 with pull-down).
	 * High R-RMS → floating DATA line during CLK HIGH (mono mic tri-states).
	 * Low R-RMS + any L-RMS → mic wired correctly, use only L channel. */
	int64_t l_sq = 0, r_sq = 0;
	uint32_t lr_count = 0;

	for (uint32_t i = mid; i < mid + m_count; i++) {
		int32_t s = samples[i];
		if (i % 2 == 0) { l_sq += s * s; } else { r_sq += s * s; }
		lr_count++;
	}
	uint32_t half = lr_count / 2;

	LOG_INF("[C0] L-ch RMS²=%u  R-ch RMS²=%u  (mid window, %u stereo pairs)",
		(uint32_t)(l_sq / (half ? half : 1)),
		(uint32_t)(r_sq / (half ? half : 1)),
		half);

	/* L/R correlation over the same mid window.
	 * corr ~ +1: channels are duplicates
	 * corr ~  0: unrelated (likely noise/floating)
	 * corr ~ -1: inverted copies
	 */
	if (half >= 8) {
		int64_t lr_dot = 0;
		int64_t ll_dot = 0;
		int64_t rr_dot = 0;

		for (uint32_t p = 0; p < half; p++) {
			uint32_t base = (mid + p) * 2;
			int32_t l = samples[base];
			int32_t r = samples[base + 1];
			lr_dot += (int64_t)l * (int64_t)r;
			ll_dot += (int64_t)l * (int64_t)l;
			rr_dot += (int64_t)r * (int64_t)r;
		}

		double denom = 1.0;
		if (ll_dot > 0 && rr_dot > 0) {
			denom = sqrt((double)ll_dot) * sqrt((double)rr_dot);
		}
		double corr = (double)lr_dot / denom;

		LOG_INF("[C0] L/R corr (mid window): %.3f", corr);
	}

	/* Stereo: 16kHz × 2 bytes/sample × 2 channels = 64000 bytes/s */
	LOG_INF("[C0] audio_byte_cnt=%u  (%.2f s at 16kHz 16-bit stereo)",
		byte_count,
		(double)byte_count / (16000.0 * 2.0 * 2.0));
}
