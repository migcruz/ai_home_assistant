/*
 * procpu — PRO CPU (Core 0)
 *
 * Controls recording via the BOOT button (GPIO0, active-low):
 *   hold  → IPM id=1 CMD_START → appcpu begins PDM capture into PSRAM
 *   release → IPM id=1 CMD_STOP → appcpu stops, sends id=2 with byte_count
 *
 * On receiving id=2 (done), procpu reads the raw PCM from shared PSRAM,
 * logs the first few sample values as a sanity check, then (TODO) builds
 * a WAV header and streams over WebSocket.
 *
 * IPM message IDs (shared with appcpu/src/main.c):
 *   id=0  appcpu → procpu   log string
 *   id=1  procpu → appcpu   command: uint8_t 1=start, 0=stop
 *   id=2  appcpu → procpu   done:    uint32_t bytes_written
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/ipm.h>
#include <zephyr/device.h>
#include <stdint.h>
#include <string.h>
#include "storage.h"

LOG_MODULE_REGISTER(procpu, LOG_LEVEL_DBG);

/* ── Shared PSRAM audio buffer ───────────────────────────────────────────── */
#define AUDIO_PSRAM_BASE  0x3C000000UL
#define AUDIO_BUF_MAX     (512U * 1024U)

static const uint8_t * const audio_buf = (const uint8_t *)AUDIO_PSRAM_BASE;

/* ── IPM IDs ─────────────────────────────────────────────────────────────── */
#define IPM_ID_LOG   0U
#define IPM_ID_CMD   1U
#define IPM_ID_DONE  2U

#define CMD_STOP  0U
#define CMD_START 1U

/* ── Hardware ────────────────────────────────────────────────────────────── */
#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

#define BTN_NODE DT_ALIAS(sw0)
static const struct gpio_dt_spec btn = GPIO_DT_SPEC_GET(BTN_NODE, gpios);

static const struct device *ipm_dev;

/* ── State ───────────────────────────────────────────────────────────────── */
static volatile bool     recording      = false;
static volatile bool     audio_ready    = false;
static volatile uint32_t audio_byte_cnt = 0;

/* ── IPM receive: log strings and done signal from appcpu ────────────────── */
static void ipm_rx_cb(const struct device *dev, void *ctx,
		      uint32_t id, volatile void *data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(ctx);

	if (id == IPM_ID_LOG) {
		LOG_INF("%s", (const char *)data);
	} else if (id == IPM_ID_DONE) {
		audio_byte_cnt = *(volatile uint32_t *)data;
		audio_ready    = true;
	}
}

/* ── Send command to appcpu via IPM ─────────────────────────────────────── */
static void send_cmd(uint8_t cmd)
{
	ipm_send(ipm_dev, 1, IPM_ID_CMD, &cmd, sizeof(cmd));
}

/* ── Sanity check: log first 8 PCM samples from PSRAM ───────────────────── */
static void log_audio_samples(uint32_t byte_count)
{
	if (byte_count < 16) {
		LOG_WRN("[C0] too few bytes to inspect (%u)", byte_count);
		return;
	}

	/* PCM is 16-bit signed little-endian */
	const int16_t *samples = (const int16_t *)audio_buf;
	uint32_t       n       = (byte_count / 2 < 8) ? byte_count / 2 : 8;

	LOG_INF("[C0] first %u PCM samples from PSRAM:", n);

	for (uint32_t i = 0; i < n; i++) {
		LOG_INF("  [%u] %d", i, samples[i]);
	}

	/* Rough RMS over first 256 samples */
	uint32_t count = (byte_count / 2 < 256) ? byte_count / 2 : 256;
	int64_t  sum_sq = 0;

	for (uint32_t i = 0; i < count; i++) {
		int32_t s = samples[i];
		sum_sq += s * s;
	}

	uint32_t rms = (uint32_t)(sum_sq / count);

	LOG_INF("[C0] RMS² over first %u samples: %u", count, rms);
	/* Stereo capture: 16kHz × 2 bytes/sample × 2 channels = 64000 bytes/s */
	LOG_INF("[C0] audio_byte_cnt=%u  (%.2f s at 16kHz 16-bit stereo)",
		byte_count,
		(double)byte_count / (16000.0 * 2.0 * 2.0));
}

/* ── Main ────────────────────────────────────────────────────────────────── */
int main(void)
{
	LOG_INF("[C0] procpu starting");

	/* Let USB enumerate before LittleFS touches flash */
	k_sleep(K_MSEC(500));
	storage_init();

	ipm_dev = DEVICE_DT_GET(DT_NODELABEL(ipm0));
	if (!device_is_ready(ipm_dev)) {
		LOG_ERR("[C0] IPM not ready");
		return -1;
	}
	ipm_register_callback(ipm_dev, ipm_rx_cb, NULL);

	if (!gpio_is_ready_dt(&led)) {
		LOG_ERR("[C0] LED GPIO not ready");
		return -1;
	}
	gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);

	if (!gpio_is_ready_dt(&btn)) {
		LOG_ERR("[C0] BOOT button GPIO not ready");
		return -1;
	}
	gpio_pin_configure_dt(&btn, GPIO_INPUT);

	LOG_INF("[C0] ready — hold BOOT button to record");

	bool btn_was_pressed = false;

	while (1) {
		/* gpio_pin_get_dt() accounts for GPIO_ACTIVE_LOW: 1 = pressed */
		bool btn_pressed = (gpio_pin_get_dt(&btn) == 1);

		if (btn_pressed && !btn_was_pressed) {
			/* Rising edge of press */
			if (!recording) {
				recording = true;
				audio_ready = false;
				LOG_INF("[C0] button pressed — sending START to appcpu");
				send_cmd(CMD_START);
				gpio_pin_set_dt(&led, 1);
			}
		} else if (!btn_pressed && btn_was_pressed) {
			/* Falling edge — button released */
			if (recording) {
				recording = false;
				LOG_INF("[C0] button released — sending STOP to appcpu");
				send_cmd(CMD_STOP);
				gpio_pin_set_dt(&led, 0);
			}
		}

		btn_was_pressed = btn_pressed;

		/* Check if appcpu signalled recording done */
		if (audio_ready) {
			audio_ready = false;
			LOG_INF("[C0] audio ready in PSRAM — %u bytes", audio_byte_cnt);
			log_audio_samples(audio_byte_cnt);
			/* TODO: build WAV header + stream over WebSocket */
		}

		k_sleep(K_MSEC(20));
	}

	return 0;
}
