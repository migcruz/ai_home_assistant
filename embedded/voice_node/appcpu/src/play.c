/*
 * play.c — I2S1 WAV playback through MAX98357A (Milestone 4, appcpu)
 *
 * appcpu owns all I2S/DMA: I2S0 for PDM mic capture, I2S1 for speaker output.
 * This avoids cross-core GDMA hardware conflicts that occur when procpu
 * initialises its own I2S driver — the ESP32-S3 GDMA global init on one core
 * corrupts the channel state the other core relies on.
 *
 * Procpu receives TTS WAV frames via WebSocket, writes them into the shared
 * PSRAM struct play_shared, flushes the cache, and sends IPM_ID_PLAY.
 * appcpu calls play_from_shared() which owns all cache invalidation,
 * magic validation, and I2S DMA feeding.
 *
 * Audio format: Piper TTS emits 22050 Hz 16-bit mono WAV.  The sample rate is
 * parsed from the WAV header each call so the server can change voices without
 * a firmware update.  Mono PCM is duplicated to stereo (L=R) before writing
 * to the I2S1 DMA.
 *
 * Pins (set in appcpu.overlay):
 *   BCLK  GPIO2 → I2S1_O_BCK
 *   LRCLK GPIO3 → I2S1_O_WS
 *   DOUT  GPIO4 → I2S1_O_SD  (into MAX98357A DIN)
 */

#include "play.h"
#include "audio_shared.h"

#include <zephyr/kernel.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/drivers/ipm.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/cache.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <limits.h>

LOG_MODULE_DECLARE(appcpu, LOG_LEVEL_DBG);

#define IPM_ID_LOG  0U

static const struct device *play_ipm;

static void play_log(const char *fmt, ...)
{
	char buf[128];
	va_list args;

	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	if (play_ipm) {
		ipm_send(play_ipm, 1, IPM_ID_LOG, buf, strlen(buf) + 1);
	}
}

#define I2S_NODE  DT_NODELABEL(i2s1)

/*
 * DMA slab in regular BSS (appcpu linker has no .ext_ram.bss).
 * BLOCK_SIZE / 4 stereo frames per block.
 * At 16kHz: 4096/4 / 16000 = 64 ms per block.
 *
 * BLOCK_COUNT=4: pre-queuing all 4 blocks before I2S_TRIGGER_START leaves 3
 * in the driver's internal queue after the first block is dequeued.  This
 * gives the main thread ~192 ms of runway to refill the queue, preventing the
 * DMA ISR from seeing an empty queue between blocks and stopping with
 * state=ERROR (which makes subsequent i2s_buf_write calls fail with -EIO).
 * Must be ≤ CONFIG_I2S_ESP32_TX_BLOCK_COUNT (default 5).
 */
#define BLOCK_SIZE   4096
#define BLOCK_COUNT  4
#define AMP_UNMUTE_DELAY_MS 20U

K_MEM_SLAB_DEFINE_STATIC(play_slab, BLOCK_SIZE, BLOCK_COUNT, 4);

static const struct device *i2s_dev;
static const struct gpio_dt_spec amp_en = GPIO_DT_SPEC_GET(DT_ALIAS(amp_en), gpios);
/* Large enough for ~1.5 s @ 16kHz mono. */
#define TONE_MAX_SAMPLES 24000U
static int16_t tone_buf[TONE_MAX_SAMPLES];

struct tts_filter_state {
	int32_t dc_x_prev;
	int32_t dc_y_prev;
	int32_t lp_y_prev;
};

struct tts_filter_config {
	/* DC blocker coefficient in Q15 (higher = lower cutoff). */
	int32_t dc_block_a_q15;
	/* One-pole LP smoothing divisor: y += (x - y) / lp_div. */
	int32_t lp_div;
	/* Noise gate threshold (absolute sample value). */
	int32_t gate_abs_threshold;
};

static const struct tts_filter_config tts_cfg = {
	.dc_block_a_q15 = 32604, /* ~0.995 */
	.lp_div = 4,             /* gentle smoothing */
	.gate_abs_threshold = 80,
};

static int16_t apply_edge_fade(int16_t s, size_t idx, size_t total, size_t fade_samples)
{
	if (total == 0 || fade_samples == 0) {
		return s;
	}

	int32_t gain_q15 = 32767;
	if (idx < fade_samples) {
		gain_q15 = (int32_t)((idx * 32767U) / fade_samples);
	}

	size_t rem = total - idx; /* rem >= 1 while idx < total */
	if (rem <= fade_samples) {
		int32_t tail_q15 = (int32_t)(((rem - 1U) * 32767U) / fade_samples);
		if (tail_q15 < gain_q15) {
			gain_q15 = tail_q15;
		}
	}

	return (int16_t)(((int32_t)s * gain_q15) / 32767);
}

/* TTS de-hiss chain:
 *  1) DC blocker (1st-order high-pass)
 *  2) Gentle low-pass to tame high-frequency hiss
 *  3) Small noise gate near zero
 */
static int16_t filter_tts_sample(int16_t s, struct tts_filter_state *st)
{
	const int32_t lp_div = (tts_cfg.lp_div > 0) ? tts_cfg.lp_div : 1;

	/* y[n] = x[n] - x[n-1] + a*y[n-1]. */
	int32_t x = s;
	int32_t y_hp = x - st->dc_x_prev +
		       ((st->dc_y_prev * tts_cfg.dc_block_a_q15) / 32767);
	st->dc_x_prev = x;
	st->dc_y_prev = y_hp;

	/* One-pole LPF: y += (x - y) / lp_div. */
	int32_t y_lp = st->lp_y_prev + ((y_hp - st->lp_y_prev) / lp_div);
	st->lp_y_prev = y_lp;

	/* Small gate to suppress idle quantization hiss. */
	if (y_lp > -tts_cfg.gate_abs_threshold &&
	    y_lp < tts_cfg.gate_abs_threshold) {
		y_lp = 0;
	}

	if (y_lp > INT16_MAX) {
		y_lp = INT16_MAX;
	} else if (y_lp < INT16_MIN) {
		y_lp = INT16_MIN;
	}

	return (int16_t)y_lp;
}

int play_init(const struct device *ipm)
{
	play_ipm = ipm;
	i2s_dev = DEVICE_DT_GET(I2S_NODE);
	if (!device_is_ready(i2s_dev)) {
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&amp_en)) {
		play_log("[C1] amp_en GPIO not ready");
		return -ENODEV;
	}

	int ret = gpio_pin_configure_dt(&amp_en, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		play_log("[C1] amp_en configure failed: %d", ret);
		return ret;
	}

	return 0;
}

static uint32_t wav_sample_rate(const uint8_t *wav, size_t len)
{
	if (len < 28 ||
	    memcmp(wav,     "RIFF", 4) != 0 ||
	    memcmp(wav + 8, "WAVE", 4) != 0) {
		return 0;
	}
	uint32_t sr;
	memcpy(&sr, wav + 24, 4);
	return sr;
}

static size_t wav_data_offset(const uint8_t *wav, size_t len)
{
	if (len < 44) {
		return 0;
	}
	for (size_t i = 12; i + 8 <= len; ) {
		uint32_t chunk_size;
		memcpy(&chunk_size, wav + i + 4, 4);
		if (memcmp(wav + i, "data", 4) == 0) {
			return i + 8;
		}
		i += 8 + chunk_size;
	}
	return 0;
}

/*
 * Core DMA feeding loop.  Configures I2S1, pre-queues BLOCK_COUNT blocks
 * before triggering START (prevents TX underrun → DMA stall → slab hang),
 * then feeds the rest.  mono[] is 16-bit mono PCM; duplicated to stereo L=R.
 */
static int play_pcm(uint32_t sample_rate, const int16_t *mono, size_t n_mono,
		    bool tts_cleanup)
{
	/* Keep amplifier muted during I2S TX startup transient. */
	(void)gpio_pin_set_dt(&amp_en, 0);

	/* Reset TX state between button-driven plays. */
	(void)i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);

	struct i2s_config cfg = {
		/* Match ESP-IDF/Arduino TX framing used with MAX98357A:
		 * 32-bit I2S slots, mono data on left slot. */
		.word_size      = 32,
		.channels       = 2,
		.format         = I2S_FMT_DATA_FORMAT_I2S,
		.options        = I2S_OPT_BIT_CLK_MASTER |
				  I2S_OPT_FRAME_CLK_MASTER,
		.frame_clk_freq = sample_rate,
		.mem_slab       = &play_slab,
		.block_size     = BLOCK_SIZE,
		.timeout        = 2000,
	};

	int ret = i2s_configure(i2s_dev, I2S_DIR_TX, &cfg);
	if (ret < 0) {
		play_log("[C1] play_pcm: i2s_configure failed: %d", ret);
		return ret;
	}

	/* i2s_buf_write() copies into driver-owned slab blocks internally. */
	int32_t tx_block[BLOCK_SIZE / 4];
	const size_t frames_per_block = BLOCK_SIZE / 8; /* 32-bit stereo frames */
	const size_t fade_samples = MAX((size_t)(sample_rate / 50U), (size_t)1U); /* 20 ms */
	struct tts_filter_state filt = {0};

	size_t src = 0;
	int pre_queued = 0;

	// Fill up a few DMA blocks to prevent TX underrun at startup
	for (int pre = 0; pre < BLOCK_COUNT && src < n_mono; pre++) {
		size_t n = MIN(frames_per_block, n_mono - src);

		for (size_t i = 0; i < n; i++) {
			int16_t s = apply_edge_fade(mono[src + i], src + i, n_mono, fade_samples);
			if (tts_cleanup) {
				s = filter_tts_sample(s, &filt);
			}
			/* Mirror mono to both slots for MAX98357A robustness. */
			tx_block[i * 2] = ((int32_t)s) << 16;
			tx_block[i * 2 + 1] = ((int32_t)s) << 16;
		}
		if (n < frames_per_block) {
			memset(&tx_block[n * 2], 0, (frames_per_block - n) * 8);
		}
		src += n;

		if (i2s_buf_write(i2s_dev, tx_block, BLOCK_SIZE) < 0) {
			play_log("[C1] play_pcm: pre-queue buf_write failed at %d", pre);
			break;
		}
		pre_queued++;
	}

	play_log("[C1] play_pcm: pre-queued=%d src=%u n_mono=%u sr=%u",
		 pre_queued, (unsigned)src, (unsigned)n_mono, (unsigned)sample_rate);

	ret = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
	if (ret < 0) {
		play_log("[C1] play_pcm: START failed: %d", ret);
		i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
		return ret;
	}

	k_sleep(K_MSEC(AMP_UNMUTE_DELAY_MS));
	(void)gpio_pin_set_dt(&amp_en, 1);

	int feed_count = 0;
	int write_errors = 0;

	// Now play the actual audio
	while (src < n_mono) {
		size_t n = MIN(frames_per_block, n_mono - src);

		for (size_t i = 0; i < n; i++) {
			int16_t s = apply_edge_fade(mono[src + i], src + i, n_mono, fade_samples);
			if (tts_cleanup) {
				s = filter_tts_sample(s, &filt);
			}
			tx_block[i * 2] = ((int32_t)s) << 16;
			tx_block[i * 2 + 1] = ((int32_t)s) << 16;
		}
		if (n < frames_per_block) {
			memset(&tx_block[n * 2], 0, (frames_per_block - n) * 8);
		}
		src += n;

		if (i2s_buf_write(i2s_dev, tx_block, BLOCK_SIZE) < 0) {
			write_errors++;
			play_log("[C1] play_pcm: buf_write error #%d at feed=%d src=%u",
				 write_errors, feed_count, (unsigned)src);
			if (write_errors >= 3) {
				break;
			}
			continue;
		}
		feed_count++;
	}

	play_log("[C1] play_pcm: done feeding feed=%d write_err=%d src=%u free=%u",
		 feed_count, write_errors,
		 (unsigned)src, (unsigned)k_mem_slab_num_free_get(&play_slab));

	i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DRAIN);

	for (int t = 0; t < 60; t++) {
		if (k_mem_slab_num_free_get(&play_slab) == BLOCK_COUNT) {
			break;
		}
		k_sleep(K_MSEC(10));
	}

	uint32_t free_after = k_mem_slab_num_free_get(&play_slab);

	play_log("[C1] play_pcm: drain done free=%u/%d", free_after, BLOCK_COUNT);
	(void)gpio_pin_set_dt(&amp_en, 0);

	/* If drain timed out (driver errored mid-playback), force-clean state. */
	if (free_after < BLOCK_COUNT) {
		i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
		/* Slab slots held by the driver are permanently lost at this point.
		 * A future call to play_pcm will fail at pre-queue if too many are
		 * leaked.  Full recovery requires a driver restart. */
		play_log("[C1] play_pcm: drain timeout — %u slots leaked",
			 BLOCK_COUNT - free_after);
	}

	return 0;
}

static int play_wav(const uint8_t *wav, uint32_t len)
{
	uint32_t sample_rate = wav_sample_rate(wav, len);
	if (sample_rate == 0) {
		return -EINVAL;
	}

	size_t data_off = wav_data_offset(wav, len);
	if (data_off == 0 || data_off >= len) {
		return -EINVAL;
	}

	return play_pcm(sample_rate,
			(const int16_t *)(wav + data_off),
			(len - data_off) / 2,
			true);
}

/*
 * Loopback test: read raw PCM directly from struct audio_shared and feed it
 * to the speaker — no WAV header construction needed.
 */
int play_audio_shared(uint32_t rec_addr, uint32_t play_buf_addr)
{
	ARG_UNUSED(play_buf_addr);

	if (rec_addr == 0) {
		return -EINVAL;
	}

	struct audio_shared *as = (struct audio_shared *)(uintptr_t)rec_addr;

	sys_cache_data_invd_range(as, sizeof(struct audio_shared));

	uint32_t byte_count  = as->byte_count;
	uint32_t sample_rate = as->sample_rate;

	if (byte_count == 0 || sample_rate == 0) {
		return -EINVAL;
	}

	sys_cache_data_invd_range(as->pcm, byte_count);

	return play_pcm(sample_rate, (const int16_t *)as->pcm, byte_count / 2, false);
}

int play_from_shared(uint32_t psram_addr)
{
	if (!i2s_dev) {
		return -ENODEV;
	}

	if (psram_addr == 0) {
		return -EINVAL;
	}

	struct play_shared *ps = (struct play_shared *)(uintptr_t)psram_addr;

	/* Invalidate cache: header first to read len, then the WAV payload. */
	sys_cache_data_invd_range(ps, sizeof(struct play_shared));
	sys_cache_data_invd_range(ps->wav, ps->len);

	if (ps->magic != PLAY_SHARED_MAGIC) {
		return -EBADMSG;
	}

	return play_wav(ps->wav, ps->len);
}

int play_test_tone(uint32_t duration_ms, uint32_t freq_hz)
{
	const uint32_t sample_rate = 16000U;

	if (duration_ms == 0U) {
		duration_ms = 1000U;
	}
	if (freq_hz == 0U) {
		freq_hz = 880U;
	}

	size_t n = ((uint64_t)sample_rate * duration_ms) / 1000U;
	if (n > TONE_MAX_SAMPLES) {
		n = TONE_MAX_SAMPLES;
	}

	/* 32-bit phase accumulator triangle wave: stable and no libm needed. */
	uint32_t phase = 0;
	uint32_t step = (uint32_t)(((uint64_t)freq_hz << 32) / sample_rate);
	const int32_t amp = 9000; /* conservative level */

	for (size_t i = 0; i < n; i++) {
		phase += step;
		uint16_t ramp = (uint16_t)(phase >> 16);
		int32_t tri = (ramp < 32768U) ? ramp : (65535U - ramp); /* 0..32767 */
		int32_t centered = (tri * 2) - 32767; /* approx -32767..32767 */
		tone_buf[i] = (int16_t)((centered * amp) / 32767);
	}

	return play_pcm(sample_rate, tone_buf, n, false);
}
