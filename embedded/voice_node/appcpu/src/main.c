/*
 * appcpu — APP CPU (Core 1) — PDM microphone capture
 *
 * Owns the PDM mic (MSM261S4030H0R, GPIO41/42 via I2S0). Writes raw PCM
 * samples into a fixed region of shared PSRAM. The procpu controls recording
 * via IPM commands and reads the buffer when done.
 *
 * IPM message IDs (shared with procpu/src/main.c):
 *   id=0  appcpu → procpu   log string   (ipm_log)
 *   id=1  procpu → appcpu   command      payload: uint8_t 1=start, 0=stop
 *   id=2  appcpu → procpu   done         payload: uint32_t bytes_written
 *
 * Shared PSRAM layout:
 *   0x3C000000 .. 0x3C07FFFF  audio buffer (512KB ≈ 16 s at 16kHz 16-bit)
 *   procpu reads this region directly after receiving id=2.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/ipm.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/device.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

/* ── Shared PSRAM audio buffer ───────────────────────────────────────────── */
#define AUDIO_PSRAM_BASE  0x3C000000UL
#define AUDIO_BUF_MAX     (512U * 1024U)   /* 512 KB ≈ 16 s at 16kHz 16-bit */

static uint8_t * const audio_buf = (uint8_t *)AUDIO_PSRAM_BASE;

/* ── IPM IDs ─────────────────────────────────────────────────────────────── */
#define IPM_ID_LOG   0U
#define IPM_ID_CMD   1U
#define IPM_ID_DONE  2U

#define CMD_STOP  0U
#define CMD_START 1U

/* ── I2S / PDM config ────────────────────────────────────────────────────── */
#define SAMPLE_RATE    16000
#define SAMPLE_WIDTH   16
/*
 * ESP32 I2S DMA always operates in stereo. channels=1 is rejected by the
 * driver with EINVAL. Mono PDM mic data arrives on the left channel; right
 * channel is zeros. We capture both and strip L/R interleaving in procpu.
 */
#define CHANNELS       2
#define BLOCK_MS       20
#define BLOCK_SAMPLES  (SAMPLE_RATE * BLOCK_MS / 1000)                       /* 320 frames */
#define BLOCK_BYTES    (BLOCK_SAMPLES * (SAMPLE_WIDTH / 8) * CHANNELS)        /* 1280 B */
#define DMA_BLOCKS     4   /* ring of DMA-owned buffers; must be >= 2 */

K_MEM_SLAB_DEFINE_STATIC(rx_slab, BLOCK_BYTES, DMA_BLOCKS, 4);

/* ── Devices ─────────────────────────────────────────────────────────────── */
static const struct device *ipm_dev;
static const struct device *i2s_dev;

/* ── State flags (set from IPM ISR, read from main loop) ─────────────────── */
static volatile bool recording    = false;
static volatile bool stop_pending = false;

/* ── ipm_log — forward a formatted string to procpu (wait=1 avoids races) ── */
static void ipm_log(const char *fmt, ...)
{
	char buf[128];
	va_list args;

	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	ipm_send(ipm_dev, 1, IPM_ID_LOG, buf, strlen(buf) + 1);
}

/* ── IPM receive callback — commands from procpu ─────────────────────────── */
static void ipm_rx_cb(const struct device *dev, void *ctx,
		      uint32_t id, volatile void *data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(ctx);

	if (id != IPM_ID_CMD) {
		return;
	}

	uint8_t cmd = *(volatile uint8_t *)data;

	if (cmd == CMD_START && !recording) {
		recording    = true;
		stop_pending = false;
	} else if (cmd == CMD_STOP && recording) {
		stop_pending = true;
	}
}

/* ── I2S PDM init ────────────────────────────────────────────────────────── */
static int i2s_pdm_init(void)
{
	i2s_dev = DEVICE_DT_GET(DT_NODELABEL(i2s0));
	if (!device_is_ready(i2s_dev)) {
		ipm_log("[C1] i2s0 not ready");
		return -ENODEV;
	}

	/*
	 * PDM RX configuration.
	 * ESP32S3 I2S in PDM mode: BCK pin drives the PDM clock (output),
	 * SD pin receives PDM data (input). The Zephyr ESP32 I2S driver
	 * enables PDM mode when the pinctrl group routes BCK as output and
	 * SD as input with no corresponding TX path.
	 *
	 * If samples look wrong (all zeros or noise), try swapping CLK/DATA
	 * GPIOs in the overlay, or adjusting format to I2S_FMT_DATA_FORMAT_I2S.
	 */
	struct i2s_config cfg = {
		.word_size      = SAMPLE_WIDTH,
		.channels       = CHANNELS,
		/* LEFT_JUSTIFIED is not supported by the ESP32 driver; use I2S */
		.format         = I2S_FMT_DATA_FORMAT_I2S,
		.options        = I2S_OPT_FRAME_CLK_MASTER | I2S_OPT_BIT_CLK_MASTER,
		.frame_clk_freq = SAMPLE_RATE,
		.mem_slab       = &rx_slab,
		.block_size     = BLOCK_BYTES,
		.timeout        = 2000,
	};

	int ret = i2s_configure(i2s_dev, I2S_DIR_RX, &cfg);

	if (ret < 0) {
		ipm_log("[C1] i2s_configure failed: %d", ret);
	}

	return ret;
}

/* ── Main ────────────────────────────────────────────────────────────────── */
int main(void)
{
	ipm_dev = DEVICE_DT_GET(DT_NODELABEL(ipm0));
	if (!device_is_ready(ipm_dev)) {
		return -1;
	}

	/* Wait for procpu USB + LittleFS init before sending first log */
	k_sleep(K_MSEC(1500));

	ipm_log("[C1] appcpu starting");
	ipm_register_callback(ipm_dev, ipm_rx_cb, NULL);

	if (i2s_pdm_init() < 0) {
		ipm_log("[C1] PDM init failed — halting");
		return -1;
	}

	ipm_log("[C1] PDM ready — waiting for start command");

	uint32_t audio_pos = 0;
	bool     i2s_running = false;

	while (1) {
		if (!recording) {
			k_sleep(K_MSEC(10));
			continue;
		}

		/* ── First block of a new recording: start I2S RX ────────── */
		if (!i2s_running) {
			int ret = i2s_trigger(i2s_dev, I2S_DIR_RX, I2S_TRIGGER_START);

			if (ret < 0) {
				ipm_log("[C1] i2s start failed: %d", ret);
				recording = false;
				continue;
			}

			i2s_running = true;
			audio_pos   = 0;
			ipm_log("[C1] recording started");
		}

		/* ── Read one DMA block ───────────────────────────────────── */
		void  *block = NULL;
		size_t block_size = 0;
		int    ret = i2s_read(i2s_dev, &block, &block_size);

		if (ret == 0 && block != NULL) {
			uint32_t space = AUDIO_BUF_MAX - audio_pos;
			uint32_t n     = (block_size < space) ? (uint32_t)block_size : space;

			memcpy(audio_buf + audio_pos, block, n);
			audio_pos += n;

			k_mem_slab_free(&rx_slab, block);

			if (audio_pos >= AUDIO_BUF_MAX) {
				ipm_log("[C1] buffer full — auto-stopping");
				stop_pending = true;
			}
		} else if (ret != -EAGAIN) {
			ipm_log("[C1] i2s_read error: %d", ret);
		}

		/* ── Stop requested (button released or buffer full) ─────── */
		if (stop_pending) {
			i2s_trigger(i2s_dev, I2S_DIR_RX, I2S_TRIGGER_STOP);
			i2s_running  = false;
			recording    = false;
			stop_pending = false;

			/* Drain any DMA blocks still in-flight */
			void *drain = NULL;
			size_t sz   = 0;

			while (i2s_read(i2s_dev, &drain, &sz) == 0 && drain != NULL) {
				k_mem_slab_free(&rx_slab, drain);
				drain = NULL;
			}

			ipm_log("[C1] recording done: %u bytes", audio_pos);

			/* Notify procpu: id=2, payload=byte_count */
			uint32_t byte_count = audio_pos;

			ipm_send(ipm_dev, 1, IPM_ID_DONE, &byte_count, sizeof(byte_count));

			audio_pos = 0;
		}
	}

	return 0;
}
