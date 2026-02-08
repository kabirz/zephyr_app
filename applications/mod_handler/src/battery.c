#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <common.h>
#include <display.h>

LOG_MODULE_REGISTER(battery_monitor, LOG_LEVEL_INF);

static const struct gpio_dt_spec charge_full =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), chargefull_gpios);
static const struct gpio_dt_spec charging = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), charging_gpios);
static const struct gpio_dt_spec power_button = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), power_gpios);
static struct gpio_callback power_button_cb_data;

/* 读取电池状态 */
static battery_status_t read_battery_status(void)
{
	int charge_full_pin = gpio_pin_get_dt(&charge_full);
	int charging_pin = gpio_pin_get_dt(&charging);

	if (charge_full_pin == 0) {
		return BATTERY_STATUS_FULL;
	}

	if (charging_pin == 0) {
		return BATTERY_STATUS_CHARGING;
	}

	return BATTERY_STATUS_DISCHARGING;
}

void power_button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)

{
	// todo
}

static int gpio_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&charge_full)) {
		LOG_ERR("Charge full GPIO device not ready");
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&charging)) {
		LOG_ERR("Charging GPIO device not ready");
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&power_button)) {
		LOG_ERR("Power button GPIO device not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&charge_full, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("Failed to configure charge_full pin: %d", ret);
		return ret;
	}

	ret = gpio_pin_configure_dt(&charging, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("Failed to configure charging pin: %d", ret);
		return ret;
	}

	ret = gpio_pin_configure_dt(&power_button, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("Failed to configure power button pin: %d", ret);
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(&power_button, GPIO_INT_EDGE_FALLING);
	if (ret < 0) {
		LOG_ERR("Failed to configure power button interrupt: %d", ret);
		return ret;
	}
	gpio_init_callback(&power_button_cb_data, power_button_pressed, BIT(power_button.pin));
	gpio_add_callback(power_button.port, &power_button_cb_data);

	LOG_INF("GPIO initialized successfully");
	LOG_INF("  PB12 (Charge Full): %s", gpio_pin_get_dt(&charge_full) ? "HIGH" : "LOW");
	LOG_INF("  PB13 (Charging):    %s", gpio_pin_get_dt(&charging) ? "HIGH" : "LOW");
	LOG_INF("  PB14 (Power Button): %s", gpio_pin_get_dt(&power_button) ? "HIGH" : "LOW");

	return 0;
}

void battery_monitor_thread(void)
{
	battery_status_t previous_status;
	int ret;

	ret = gpio_init();
	if (ret < 0) {
		LOG_ERR("GPIO initialization failed: %d", ret);
		return;
	}

	previous_status = read_battery_status();
	global_params.battery_status = previous_status;

	while (1) {
		k_event_wait(&global_params.event, TIMEOUT_EVENT, false, K_FOREVER);
		global_params.battery_status = read_battery_status();

		if (global_params.battery_status != previous_status) {
			mod_display_battery(&global_params);
		}

		k_sleep(K_SECONDS(5));
	}
}

K_THREAD_DEFINE(battery_monitor_tid, 1024, battery_monitor_thread, NULL, NULL, NULL, 7, 0, 0);
