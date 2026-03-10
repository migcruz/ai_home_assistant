#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/ipm.h>
#include <zephyr/device.h>
#include "storage.h"

LOG_MODULE_REGISTER(procpu, LOG_LEVEL_DBG);

#define LED0_NODE DT_ALIAS(led0)
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

static void ipm_rx_cb(const struct device *dev, void *ctx,
		      uint32_t id, volatile void *data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(ctx);
	ARG_UNUSED(id);

	// LOG_INF("[C1] %s", (const char *)data);
}

int main(void)
{
	LOG_INF("[C0] procpu starting");
	/* ESP32S3: flash writes (e.g. LittleFS format) temporarily disable the
	 * instruction cache. If this races with USB Serial/JTAG enumeration the
	 * USB connection dies. Wait for USB to finish enumerating first. */
	k_sleep(K_MSEC(500));

	storage_init();

	const struct device *ipm = DEVICE_DT_GET(DT_NODELABEL(ipm0));

	if (!device_is_ready(ipm)) {
		LOG_ERR("[C0] IPM device not ready");
		return -1;
	}
	ipm_register_callback(ipm, ipm_rx_cb, NULL);

	if (!gpio_is_ready_dt(&led)) {
		LOG_ERR("[C0] LED GPIO not ready");
		return -1;
	}
	gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);

	LOG_INF("[C0] running — LED blinking, waiting for appcpu heartbeats");

	while (1) {
		gpio_pin_toggle_dt(&led);
		// LOG_INF("[C0] alive, uptime %lld ms", k_uptime_get());
		k_sleep(K_MSEC(1000));
	}

	return 0;
}

// TODO make watchdog on procpu to check that appcpu is still alive