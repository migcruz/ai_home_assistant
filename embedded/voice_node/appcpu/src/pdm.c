#include "pdm.h"

#include <zephyr/kernel.h>
#include <zephyr/cache.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/drivers/ipm.h>
#include <zephyr/device.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

/* ESP-IDF HAL/LL headers for PDM RX mode.
 * Zephyr's ESP32 I2S driver only supports standard I2S framing; it has no
 * PDM mode.  After i2s_configure() we patch the hardware registers directly
 * to enable the on-chip PDM→PCM decimation filter.
 */
#include <soc/i2s_struct.h>
#include <hal/i2s_ll.h>
#include <hal/i2s_types.h>

#define IPM_ID_LOG  0U

#define BLOCK_SAMPLES  (AUDIO_SAMPLE_RATE * BLOCK_MS / 1000)                  /* 320 frames */
#define BLOCK_BYTES    (BLOCK_SAMPLES * (SAMPLE_WIDTH / 8) * CHANNELS)  /* 1280 B     */
#define DMA_BLOCKS     4

K_MEM_SLAB_DEFINE_STATIC(rx_slab, BLOCK_BYTES, DMA_BLOCKS, 4);

static uint8_t *audio_buf = (uint8_t *)AUDIO_PSRAM_BASE;

void pdm_set_audio_buf(uint32_t addr)
{
	audio_buf = (uint8_t *)(uintptr_t)addr;
}

static const struct device *i2s_dev;
static const struct device *ipm_dev;

/* PDM slot mask for ESP32-S3 PDM RX: BIT0=right, BIT1=left. */
#ifndef PDM_ACTIVE_MASK
/* Keep parity with testapp_mic: enable both slots at RX and pick slot later. */
#define PDM_ACTIVE_MASK 0x03
#endif

static void pdm_log(const char *fmt, ...)
{
	char buf[128];
	va_list args;

	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	ipm_send(ipm_dev, 1, IPM_ID_LOG, buf, strlen(buf) + 1);
}

static void log_block_stats(const int16_t *samples, size_t count, uint32_t block_idx)
{
	int64_t l_sum = 0, l_sum_sq = 0;
	int64_t r_sum = 0, r_sum_sq = 0;
	uint32_t n = 0;
	int16_t l_min = 32767, l_max = -32768;
	int16_t r_min = 32767, r_max = -32768;
	int16_t first_non_trivial = 0;
	bool have_first = false;

	for (size_t i = 0; i + 1 < count; i += 2) {
		int32_t l = samples[i];
		int32_t r = samples[i + 1];

		l_sum += l;
		l_sum_sq += (int64_t)l * (int64_t)l;
		r_sum += r;
		r_sum_sq += (int64_t)r * (int64_t)r;
		n++;

		if (l < l_min) {
			l_min = (int16_t)l;
		}
		if (l > l_max) {
			l_max = (int16_t)l;
		}
		if (r < r_min) {
			r_min = (int16_t)r;
		}
		if (r > r_max) {
			r_max = (int16_t)r;
		}

		if (!have_first && l != 0 && l != -1 && l != 1) {
			first_non_trivial = (int16_t)l;
			have_first = true;
		}
	}

	if (n == 0U) {
		return;
	}

	int32_t l_mean = (int32_t)(l_sum / (int64_t)n);
	int32_t r_mean = (int32_t)(r_sum / (int64_t)n);
	uint32_t l_rms2 = (uint32_t)(l_sum_sq / (int64_t)n);
	uint32_t r_rms2 = (uint32_t)(r_sum_sq / (int64_t)n);
	int32_t l_p2p = (int32_t)l_max - (int32_t)l_min;
	int32_t r_p2p = (int32_t)r_max - (int32_t)r_min;
	int16_t s16 = have_first ? first_non_trivial : 0;
	uint16_t u16 = (uint16_t)s16;

	pdm_log("[C1] blk=%u n=%u L(mean=%d rms2=%u min=%d max=%d p2p=%d) R(mean=%d rms2=%u min=%d max=%d p2p=%d) sample_s16=%d sample_u16=%u sample_hex=0x%04x",
		block_idx, n,
		l_mean, l_rms2, l_min, l_max, l_p2p,
		r_mean, r_rms2, r_min, r_max, r_p2p,
		s16, u16, u16);
}

/* Keep this register patch sequence in parity with testapp_mic apply_pdm_patch(). */
static void apply_pdm_patch(void)
{
	/* Re-apply RX config after i2s_trigger(START), which resets RX mode. */
	I2S0.rx_conf.rx_start = 0;

	i2s_ll_rx_reset(&I2S0);
	i2s_ll_rx_set_slave_mod(&I2S0, false);
	i2s_ll_rx_set_sample_bit(&I2S0, 16, 16);
	i2s_ll_rx_set_half_sample_bit(&I2S0, 16);
	i2s_ll_rx_enable_mono_mode(&I2S0, false);
	i2s_ll_rx_set_active_chan_mask(&I2S0, PDM_ACTIVE_MASK);
	i2s_ll_rx_enable_pdm(&I2S0);

	/* Be explicit about PDM mode bits and PDM2PCM settings. */
	I2S0.rx_conf.rx_tdm_en = 0;
	I2S0.rx_conf.rx_pdm_en = 1;
	I2S0.rx_conf.rx_pdm2pcm_en = 1;
	i2s_ll_rx_set_pdm_dsr(&I2S0, I2S_PDM_DSR_8S); /* 64x downsample */

	/* Keep sample format deterministic. */
	i2s_ll_rx_enable_big_endian(&I2S0, false);
	i2s_ll_rx_enable_left_align(&I2S0, false);
	I2S0.rx_conf.rx_bit_order = 0;
	I2S0.rx_conf.rx_pcm_conf = 0;
	I2S0.rx_conf.rx_pcm_bypass = 1;

	i2s_ll_rx_start(&I2S0);
}

int pdm_init(const struct device *ipm)
{
	ipm_dev = ipm;
	i2s_dev = DEVICE_DT_GET(DT_NODELABEL(i2s0));

	if (!device_is_ready(i2s_dev)) {
		pdm_log("[C1] i2s0 not ready");
		return -ENODEV;
	}

	/*
	 * Zephyr's ESP32 I2S driver does not support PDM mode — it only
	 * handles standard I2S framing.  We use it here solely for DMA setup
	 * and clock generation, then patch the hardware into PDM RX mode below.
	 *
	 * Clock: the driver computes BCK = frame_clk_freq × channels × word_size.
	 * In PDM mode (rx_pdm_en=1) the ESP32-S3 hardware outputs the PDM bit
	 * clock on the WS signal (I2S0I_WS_OUT_IDX=27, GPIO42 via overlay) and
	 * reads incoming data against that same WS reference.  BCK (signal 26)
	 * is unused; rx_tdm_ws_width is bypassed and WS carries the PDM bit
	 * clock at the BCK-equivalent frequency (~1.053 MHz).
	 * Measured register values (rx_conf1, rx_clkm_conf):
	 *   MCLK = 160MHz / 19 = 8.421 MHz
	 *   PDM clock (WS in PDM mode) = 8.421MHz / 8 = 1.053 MHz  ✓
	 * The MSM261S4030H0R requires 1–3.25 MHz: 1.053 MHz is within spec.
	 * The hardware PDM2PCM decimator (64×) produces PCM at ~16.4 kHz.
	 * Setting frame_clk_freq = AUDIO_SAMPLE_RATE * 2 gives:
	 *   target BCK = 32000 × 2 × 16 = 1,024,000 Hz (actual: 1,053,000 due to
	 *   integer clock division — acceptable, within mic spec).
	 *
	 * channels=2: required by the DMA controller; mono PDM mic data
	 * arrives on the left channel, right channel is zeros.
	 */
	struct i2s_config cfg = {
		.word_size      = SAMPLE_WIDTH,
		.channels       = CHANNELS,
		.format         = I2S_FMT_DATA_FORMAT_I2S,
		.options        = I2S_OPT_FRAME_CLK_MASTER | I2S_OPT_BIT_CLK_MASTER,
		.frame_clk_freq = AUDIO_SAMPLE_RATE * 2, /* sets BCK = 1.024 MHz for PDM */
		.mem_slab       = &rx_slab,
		.block_size     = BLOCK_BYTES,
		.timeout        = BLOCK_MS * 2, /* 40 ms: 2× block period, fast stop response */
	};

	int ret = i2s_configure(i2s_dev, I2S_DIR_RX, &cfg);

	if (ret < 0) {
		pdm_log("[C1] i2s_configure failed: %d", ret);
		return ret;
	}

	pdm_log("[C1] i2s configured; PDM register patch will be applied on START");
	return 0;
}

uint32_t pdm_record(volatile bool *stop_flag)
{
	uint32_t audio_pos = 0;
	uint32_t block_idx = 0;
	uint32_t eio_recoveries = 0;
	bool recovered_this_capture = false;

	/* Write the shared header into the buffer before recording starts.
	 * byte_count is filled in after the loop once we know the final size. */
	struct audio_shared *shdr = (struct audio_shared *)audio_buf;

	shdr->magic       = AUDIO_SHARED_MAGIC;
	shdr->byte_count  = 0;
	shdr->sample_rate = AUDIO_SAMPLE_RATE;
	shdr->channels    = AUDIO_CHANNELS;
	shdr->bits        = AUDIO_BITS;

	/* Ensure RX queue/state is clean between button-driven captures. */
	(void)i2s_trigger(i2s_dev, I2S_DIR_RX, I2S_TRIGGER_DROP);

	int ret = i2s_trigger(i2s_dev, I2S_DIR_RX, I2S_TRIGGER_START);

	if (ret < 0) {
		pdm_log("[C1] i2s start failed: %d", ret);
		return 0;
	}

	/*
	 * i2s_trigger(START) calls i2s_hal_rx_reset() internally which, on
	 * ESP32-S3, resets the I2S RX operating mode (clears rx_pdm_en etc.)
	 * back to hardware defaults.  The Zephyr driver then commits standard
	 * I2S mode via rx_update.
	 *
	 * Replicate the full ESP-IDF i2s_hal_pdm_set_rx_slot() + enable +
	 * start sequence to switch the hardware to PDM RX mode:
	 *   1. Stop I2S RX (GDMA stays armed; just pauses FIFO drain).
	 *   2. Reset RX module to a clean slate.
	 *   3. Re-apply all RX slot config for PDM (ESP32-S3 HW v2 path).
	 *   4. Enable PDM→PCM conversion.
	 *   5. i2s_ll_rx_start(): rx_update=1 (clocks config into RX domain),
	 *      wait for hw ack, rx_start=1.
	 */
	apply_pdm_patch();

	/*
	 * Dump registers AFTER rx_start so we see the in-effect RX domain config.
	 * If rx_pdm2pcm_en is 0 here, audio will be white noise (raw 1-bit PDM).
	 */
	pdm_log("[C1] rx_conf=0x%08X (tdm=%u pdm=%u pdm2pcm=%u dsr128=%u) rx_conf1=0x%08X clkm=0x%08X",
		I2S0.rx_conf.val,
		I2S0.rx_conf.rx_tdm_en,
		I2S0.rx_conf.rx_pdm_en,
		I2S0.rx_conf.rx_pdm2pcm_en,
		I2S0.rx_conf.rx_pdm_sinc_dsr_16_en,
		I2S0.rx_conf1.val,
		I2S0.rx_clkm_conf.val);
	pdm_log("[C1] PDM_ACTIVE_MASK=0x%02X (right=%u left=%u)",
		PDM_ACTIVE_MASK,
		(PDM_ACTIVE_MASK & 0x01) ? 1 : 0,
		(PDM_ACTIVE_MASK & 0x02) ? 1 : 0);

	pdm_log("[C1] recording started");

	while (!(*stop_flag)) {
		void  *block     = NULL;
		size_t block_size = 0;

		ret = i2s_read(i2s_dev, &block, &block_size);

		if (ret == 0 && block != NULL) {
			const int16_t *samples = (const int16_t *)block;
			uint32_t sample_count = block_size / sizeof(int16_t);
			uint32_t stereo_pairs = sample_count / 2U;
			uint32_t mono_space_pairs = (AUDIO_PCM_MAX - audio_pos) /
						    sizeof(int16_t);
			uint32_t frames = stereo_pairs;

			if (frames > mono_space_pairs) {
				frames = mono_space_pairs;
			}

			// if ((block_idx % 10U) == 0U) {
			// 	log_block_stats((const int16_t *)block, block_size / sizeof(int16_t),
			// 			block_idx);
			// }

			/* Per-block mono conversion: keep LEFT slot only. */
			int16_t *mono_dst = (int16_t *)(shdr->pcm + audio_pos);
			for (uint32_t i = 0; i < frames; i++) {
				mono_dst[i] = samples[2U * i];
			}

			audio_pos += frames * sizeof(int16_t);
			k_mem_slab_free(&rx_slab, block);
			block_idx++;

			if (frames < stereo_pairs || audio_pos >= AUDIO_PCM_MAX) {
				pdm_log("[C1] buffer full — auto-stopping (pcm=%u)",
					audio_pos);
				break;
			}
		} else if (ret != -EAGAIN) {
			if (ret == -EIO && !recovered_this_capture && !(*stop_flag)) {
				eio_recoveries++;
				pdm_log("[C1] i2s_read EIO; attempting RX recovery (%u)",
					eio_recoveries);
				(void)i2s_trigger(i2s_dev, I2S_DIR_RX, I2S_TRIGGER_DROP);
				ret = i2s_trigger(i2s_dev, I2S_DIR_RX, I2S_TRIGGER_START);
				if (ret < 0) {
					pdm_log("[C1] RX recovery START failed: %d", ret);
					break;
				}
				apply_pdm_patch();
				recovered_this_capture = true;
				continue;
			}
			pdm_log("[C1] i2s_read error: %d", ret);
		}
	}

	i2s_trigger(i2s_dev, I2S_DIR_RX, I2S_TRIGGER_STOP);

	/* Drain any in-flight DMA blocks */
	void  *drain = NULL;
	size_t sz    = 0;

	while (i2s_read(i2s_dev, &drain, &sz) == 0 && drain != NULL) {
		k_mem_slab_free(&rx_slab, drain);
		drain = NULL;
	}

	/* Fill in byte_count now that we know the final PCM size. */
	shdr->byte_count = audio_pos;

	/* Flush header + PCM. */
	sys_cache_data_flush_range((void *)audio_buf,
				   sizeof(struct audio_shared) + audio_pos);

	pdm_log("[C1] recording done: pcm=%u bytes (eio_recoveries=%u)",
		audio_pos, eio_recoveries);
	return audio_pos;
}
