#include "pdm.h"

#include <zephyr/kernel.h>
#include <zephyr/cache.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/drivers/ipm.h>
#include <zephyr/device.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

#define IPM_ID_LOG  0U

#define BLOCK_SAMPLES  (SAMPLE_RATE * BLOCK_MS / 1000)                  /* 320 frames */
#define BLOCK_BYTES    (BLOCK_SAMPLES * (SAMPLE_WIDTH / 8) * CHANNELS)  /* 1280 B     */
#define DMA_BLOCKS     4

K_MEM_SLAB_DEFINE_STATIC(rx_slab, BLOCK_BYTES, DMA_BLOCKS, 4);

static uint8_t * const audio_buf = (uint8_t *)AUDIO_PSRAM_BASE;

static const struct device *i2s_dev;
static const struct device *ipm_dev;

static void pdm_log(const char *fmt, ...)
{
	char buf[128];
	va_list args;

	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	ipm_send(ipm_dev, 1, IPM_ID_LOG, buf, strlen(buf) + 1);
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
	 * ESP32 I2S in PDM RX mode. channels=2 is required by the DMA
	 * controller; mono mic data arrives on the left channel.
	 * LEFT_JUSTIFIED is rejected by the driver — use I2S framing.
	 */
	struct i2s_config cfg = {
		.word_size      = SAMPLE_WIDTH,
		.channels       = CHANNELS,
		.format         = I2S_FMT_DATA_FORMAT_I2S,
		.options        = I2S_OPT_FRAME_CLK_MASTER | I2S_OPT_BIT_CLK_MASTER,
		.frame_clk_freq = SAMPLE_RATE,
		.mem_slab       = &rx_slab,
		.block_size     = BLOCK_BYTES,
		.timeout        = 2000,
	};

	int ret = i2s_configure(i2s_dev, I2S_DIR_RX, &cfg);

	if (ret < 0) {
		pdm_log("[C1] i2s_configure failed: %d", ret);
	}

	return ret;
}

uint32_t pdm_record(volatile bool *stop_flag)
{
	uint32_t audio_pos = 0;

	int ret = i2s_trigger(i2s_dev, I2S_DIR_RX, I2S_TRIGGER_START);

	if (ret < 0) {
		pdm_log("[C1] i2s start failed: %d", ret);
		return 0;
	}

	pdm_log("[C1] recording started");

	while (!(*stop_flag)) {
		void  *block     = NULL;
		size_t block_size = 0;

		ret = i2s_read(i2s_dev, &block, &block_size);

		if (ret == 0 && block != NULL) {
			uint32_t space = AUDIO_BUF_MAX - audio_pos;
			uint32_t n     = (block_size < space) ? (uint32_t)block_size : space;

			memcpy(audio_buf + audio_pos, block, n);
			audio_pos += n;
			k_mem_slab_free(&rx_slab, block);

			if (audio_pos >= AUDIO_BUF_MAX) {
				pdm_log("[C1] buffer full — auto-stopping");
				break;
			}
		} else if (ret != -EAGAIN) {
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

	/* Flush appcpu's data cache so procpu sees the written PCM via PSRAM */
	sys_cache_data_flush_range((void *)audio_buf, audio_pos);

	pdm_log("[C1] recording done: %u bytes", audio_pos);
	return audio_pos;
}
