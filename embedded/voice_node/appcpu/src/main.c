/*
 * appcpu — APP CPU (Core 1)
 *
 * Waits for CMD_START from procpu via IPM, captures PDM audio into shared
 * PSRAM using pdm_record(), then signals procpu via IPM with the byte count.
 *
 * IPM message IDs (shared with procpu/src/main.c):
 *   id=0  appcpu → procpu   log string
 *   id=1  procpu → appcpu   command: uint8_t 1=start, 0=stop
 *   id=2  appcpu → procpu   done:    uint32_t bytes_written
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/ipm.h>
#include <zephyr/device.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "pdm.h"

#define IPM_ID_LOG   0U
#define IPM_ID_CMD   1U
#define IPM_ID_DONE  2U

#define CMD_STOP   0U
#define CMD_START  1U

static const struct device *ipm_dev;

static volatile bool recording    = false;
static volatile bool stop_pending = false;

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

		uint32_t byte_count = pdm_record(&stop_pending);

		recording    = false;
		stop_pending = false;

		ipm_send(ipm_dev, 1, IPM_ID_DONE, &byte_count, sizeof(byte_count));
	}

	return 0;
}
