#include "button.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/ipm.h>

LOG_MODULE_DECLARE(procpu, LOG_LEVEL_DBG);

#define IPM_ID_CMD  1U
#define CMD_STOP    0U
#define CMD_START   1U

#define BTN_NODE DT_ALIAS(sw0)
static const struct gpio_dt_spec btn = GPIO_DT_SPEC_GET(BTN_NODE, gpios);

static struct gpio_callback  btn_cb_data;
static struct k_work         btn_work;
static const struct device  *ipm;

static volatile bool recording = false;

static void send_cmd(uint8_t cmd)
{
	ipm_send(ipm, 1, IPM_ID_CMD, &cmd, sizeof(cmd));
}

static void btn_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	bool pressed = (gpio_pin_get_dt(&btn) == 1);

	if (pressed && !recording) {
		recording = true;
		LOG_INF("[C0] button pressed — sending START to appcpu");
		send_cmd(CMD_START);
	} else if (!pressed && recording) {
		recording = false;
		LOG_INF("[C0] button released — sending STOP to appcpu");
		send_cmd(CMD_STOP);
	}
}

static void btn_isr(const struct device *dev, struct gpio_callback *cb,
		    uint32_t pins)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	k_work_submit(&btn_work);
}

int button_init(const struct device *ipm_dev)
{
	ipm = ipm_dev;

	if (!gpio_is_ready_dt(&btn)) {
		LOG_ERR("[C0] BOOT button GPIO not ready");
		return -ENODEV;
	}

	gpio_pin_configure_dt(&btn, GPIO_INPUT);
	k_work_init(&btn_work, btn_work_handler);
	gpio_init_callback(&btn_cb_data, btn_isr, BIT(btn.pin));
	gpio_add_callback(btn.port, &btn_cb_data);
	gpio_pin_interrupt_configure_dt(&btn, GPIO_INT_EDGE_BOTH);

	return 0;
}
