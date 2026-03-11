#pragma once

#include <zephyr/drivers/ipm.h>

/*
 * Initialise the BOOT button (GPIO0) with interrupt-driven edge detection.
 * Both press and release are handled via a k_work item in the system workqueue.
 * ipm_dev is used to send CMD_START / CMD_STOP to appcpu.
 */
int button_init(const struct device *ipm_dev);
