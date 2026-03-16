/*
 * appcpu — APP CPU (Core 1)
 *
 * Waits for CMD_START from procpu via IPM, captures PDM audio into shared
 * PSRAM using pdm_record(), then signals procpu via IPM with the byte count.
 *
 * IPM message IDs (shared with procpu/src/main.c):
 *   id=0  appcpu → procpu   log string
 *   id=1  procpu → appcpu   command: uint8_t 1=start, 0=stop
 *   id=2  appcpu → procpu   done:    signal only (byte_count lives in struct audio_shared)
 *   id=3  procpu → appcpu   buf_addr: uint32_t address of audio_psram_buf
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/ipm.h>
#include <zephyr/device.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "pdm.h"
#include "ipm_ids.h"

static const struct device *ipm_dev;

static volatile bool     recording    = false;
static volatile bool     stop_pending = false;
static volatile uint32_t audio_buf_addr = 0;

static void ipm_log(const char *fmt, ...)
{
	char buf[128];
	va_list args;

	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	ipm_send(ipm_dev, 1, IPM_ID_LOG, buf, strlen(buf) + 1);
}

static void ipm_rx_cb(const struct device *dev, void *ctx,
		      uint32_t id, volatile void *data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(ctx);

	if (id == IPM_ID_CMD) {
		uint8_t cmd = *(volatile uint8_t *)data;

		if (cmd == CMD_START && !recording) {
			recording    = true;
			stop_pending = false;
		} else if (cmd == CMD_STOP && recording) {
			stop_pending = true;
		}
	} else if (id == IPM_ID_BUFADDR) {
		audio_buf_addr = *(volatile uint32_t *)data;
	}
}

int main(void)
{
	ipm_dev = DEVICE_DT_GET(DT_NODELABEL(ipm0));
	if (!device_is_ready(ipm_dev)) {
		return -1;
	}

	/* Register callback early so we catch IPM_ID_BUFADDR from procpu
	 * before it arrives (procpu sends it ~500ms after boot). */
	ipm_register_callback(ipm_dev, ipm_rx_cb, NULL);

	/* Wait for procpu USB + LittleFS init before sending first log */
	k_sleep(K_MSEC(1500));

	ipm_log("[C1] appcpu starting");

	/* Wait for procpu to send the PSRAM audio buffer address (ID=3).
	 * Timeout after 5 s — if it never arrives, fall back to the
	 * compile-time default baked into pdm.h (AUDIO_PSRAM_BASE). */
	for (int i = 0; i < 500 && audio_buf_addr == 0; i++) {
		k_sleep(K_MSEC(10));
	}

	if (audio_buf_addr != 0) {
		pdm_set_audio_buf(audio_buf_addr);
		ipm_log("[C1] audio buf addr: 0x%08X", audio_buf_addr);
	} else {
		ipm_log("[C1] WARN: buf addr not received, using default 0x%08X",
			AUDIO_PSRAM_BASE);
	}

	if (pdm_init(ipm_dev) < 0) {
		ipm_log("[C1] PDM init failed — halting");
		return -1;
	}

	ipm_log("[C1] PDM ready — waiting for start command");

	while (1) {
		if (!recording) {
			k_sleep(K_MSEC(10));
			continue;
		}

		(void)pdm_record(&stop_pending);

		recording    = false;
		stop_pending = false;

		/* Signal procpu that audio is ready; byte_count lives in the
		 * struct audio_shared header already flushed to PSRAM. */
		uint8_t done = 1U;
		ipm_send(ipm_dev, 1, IPM_ID_DONE, &done, sizeof(done));
	}

	return 0;
}
