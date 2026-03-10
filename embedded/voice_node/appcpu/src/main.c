#include <zephyr/kernel.h>
#include <zephyr/drivers/ipm.h>
#include <zephyr/device.h>
#include <stdio.h>
#include <string.h>

/*
 * Forward a log string to the procpu via IPM (channel 0, wait=1).
 * wait=1 blocks until the procpu has consumed the message so consecutive
 * calls never overwrite each other in the single shared-memory slot.
 * The procpu callback prints it with a [C1] prefix.
 */
static void ipm_log(const struct device *ipm, const char *fmt, ...)
{
	char buf[128];
	va_list args;

	va_start(args, fmt);
	vsnprintf(buf, sizeof(buf), fmt, args);
	va_end(args);

	ipm_send(ipm, 1, 0, buf, strlen(buf) + 1);
}

int main(void)
{
	const struct device *ipm = DEVICE_DT_GET(DT_NODELABEL(ipm0));

	if (!device_is_ready(ipm)) {
		return -1;
	}

	// Wait 1 second for LittleFS and USB to intialize on procpu
	k_sleep(K_MSEC(1000));

	ipm_log(ipm, "appcpu starting");

	uint32_t tick = 0;

	while (1) {
		// ipm_log(ipm, "alive tick=%u uptime=%lld ms", tick, k_uptime_get());
		tick++;
		k_sleep(K_MSEC(2000));
	}

	return 0;
}
