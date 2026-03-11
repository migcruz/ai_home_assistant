#include "audio.h"

#include <zephyr/kernel.h>
#include <zephyr/cache.h>
#include <zephyr/logging/log.h>
#include <stdint.h>

LOG_MODULE_DECLARE(procpu, LOG_LEVEL_DBG);

const uint8_t * const audio_buf = (const uint8_t *)AUDIO_PSRAM_BASE;

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

	/* Rough RMS² over first 256 samples */
	uint32_t count  = (byte_count / 2 < 256) ? byte_count / 2 : 256;
	int64_t  sum_sq = 0;

	for (uint32_t i = 0; i < count; i++) {
		int32_t s = samples[i];
		sum_sq += s * s;
	}

	LOG_INF("[C0] RMS² over first %u samples: %u", count,
		(uint32_t)(sum_sq / count));

	/* Stereo: 16kHz × 2 bytes/sample × 2 channels = 64000 bytes/s */
	LOG_INF("[C0] audio_byte_cnt=%u  (%.2f s at 16kHz 16-bit stereo)",
		byte_count,
		(double)byte_count / (16000.0 * 2.0 * 2.0));
}
