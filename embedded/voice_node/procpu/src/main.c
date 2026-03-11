/*
 * procpu — PRO CPU (Core 0)
 *
 * Drives the BOOT button (interrupt-driven) and waits for audio done from
 * appcpu. On done, reads PCM from shared PSRAM for sanity logging.
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
#include "storage.h"
#include "button.h"
#include "audio.h"

LOG_MODULE_REGISTER(procpu, LOG_LEVEL_DBG);

#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

#define IPM_ID_LOG   0U
#define IPM_ID_DONE  2U

static const struct device *ipm_dev;

static volatile bool     audio_ready    = false;
static volatile uint32_t audio_byte_cnt = 0;

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

int main(void)
{
	LOG_INF("[C0] procpu starting");

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
	gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE);

	if (button_init(ipm_dev) < 0) {
		return -1;
	}

	LOG_INF("[C0] ready — hold BOOT button to record");

	int blink_ticks = 0;

	while (1) {
		if (audio_ready) {
			audio_ready = false;
			LOG_INF("[C0] audio ready in PSRAM — %u bytes", audio_byte_cnt);
			log_audio_samples(audio_byte_cnt);
			/* TODO: build WAV header + stream over WebSocket */
		}

		if (++blink_ticks >= 25) {
			gpio_pin_toggle_dt(&led);
			blink_ticks = 0;
		}

		k_sleep(K_MSEC(20));
	}

	return 0;
}
