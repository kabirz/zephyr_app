#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <common.h>
#include <display.h>
#include <mod-can.h>
#include <lora.h>
#include <power.h>

LOG_MODULE_REGISTER(battery_monitor, LOG_LEVEL_INF);

static const struct gpio_dt_spec charge_full = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), chargefull_gpios);
static const struct gpio_dt_spec charging = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), charging_gpios);
static const struct gpio_dt_spec power_button = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), power_gpios);
static const struct gpio_dt_spec handle_button = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), handlebt_gpios);
static const struct gpio_dt_spec link_switch = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), linksw_gpios);
static struct gpio_callback power_button_cb_data;
static struct gpio_callback linksw_cb_data;

static struct k_work btn_display_work;
static struct k_work linksw_work;
static struct k_work sleep_work;

static void btn_display_work_handler(struct k_work *work)
{
	mod_display_handler_button(global_params.h_button);

	if (global_params.connect_type == CAN_TYPE) {
		mod_can_send_handler_state(&global_params);
	} else {
		lora_send_telemetry(&global_params);
	}
}

static void linksw_work_handler(struct k_work *work)
{
	global_params.connect_type = (global_params.connect_type == CAN_TYPE) ? LORA_TYPE : CAN_TYPE;
	mod_display_lora_can(global_params.connect_type);
	LOG_INF("Link switch: %s", global_params.connect_type == CAN_TYPE ? "CAN" : "LoRa");
}

static void system_sleep(void)
{
	can_power_enable(false);
	lora_power_enable(false);
	dis_power_enable(false);
	p5_power_enable(false);
	k_event_clear(&global_params.event, WAKE_EVENT);
	global_params.sleeping = true;
	LOG_INF("system entering sleep");
}

static void system_wake(void)
{
	p5_power_enable(true);
	dis_power_enable(true);
	can_power_enable(true);
	lora_power_enable(true);
	global_params.sleeping = false;
	last_activity_time = k_uptime_get_32();
	k_event_set(&global_params.event, WAKE_EVENT);
	k_msleep(100);
	mod_display_clear();
	mod_display_all(&global_params);
	LOG_INF("system woke up");
}

static void sleep_work_handler(struct k_work *work)
{
	if (global_params.sleeping) {
		system_wake();
	} else {
		system_sleep();
	}
}

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

void gpio_irq(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	if (pins & BIT(power_button.pin)) {
		k_work_submit(&sleep_work);
	} else if (pins & BIT(handle_button.pin)) {
		if (global_params.sleeping) {
			return;
		}
		LOG_INF("handler button Press!");
		if (gpio_pin_get_dt(&handle_button) != global_params.h_button) {
			global_params.h_button = !global_params.h_button;
			last_activity_time = k_uptime_get_32();
			k_work_submit(&btn_display_work);
		}
	}
}

static void linksw_irq(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	if (global_params.sleeping) {
		return;
	}
	k_work_submit(&linksw_work);
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

	if (!gpio_is_ready_dt(&handle_button)) {
		LOG_ERR("handle button GPIO device not ready");
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&link_switch)) {
		LOG_ERR("link switch GPIO device not ready");
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

	ret = gpio_pin_configure_dt(&handle_button, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("Failed to configure handle button pin: %d", ret);
		return ret;
	}

	ret = gpio_pin_configure_dt(&link_switch, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("Failed to configure link switch pin: %d", ret);
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(&power_button, GPIO_INT_EDGE_FALLING);
	if (ret < 0) {
		LOG_ERR("Failed to configure power button interrupt: %d", ret);
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(&handle_button, GPIO_INT_EDGE_FALLING);
	if (ret < 0) {
		LOG_ERR("Failed to configure power button interrupt: %d", ret);
		return ret;
	}
	gpio_init_callback(&power_button_cb_data, gpio_irq,
				   BIT(power_button.pin) | BIT(handle_button.pin));
	gpio_add_callback(power_button.port, &power_button_cb_data);

	ret = gpio_pin_interrupt_configure_dt(&link_switch, GPIO_INT_EDGE_FALLING);
	if (ret < 0) {
		LOG_ERR("Failed to configure link switch interrupt: %d", ret);
		return ret;
	}
	gpio_init_callback(&linksw_cb_data, linksw_irq, BIT(link_switch.pin));
	gpio_add_callback(link_switch.port, &linksw_cb_data);

	k_work_init(&btn_display_work, btn_display_work_handler);
	k_work_init(&linksw_work, linksw_work_handler);
	k_work_init(&sleep_work, sleep_work_handler);

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
		k_event_wait(&global_params.event, TIMEOUT_EVENT | WAKE_EVENT, false, K_FOREVER);
		if (global_params.sleeping) {
			continue;
		}

		global_params.battery_status = read_battery_status();

		if (global_params.battery_status != previous_status) {
			previous_status = global_params.battery_status;
		}

		k_sleep(K_SECONDS(5));
	}
}

K_THREAD_DEFINE(battery_monitor_tid, 1024, battery_monitor_thread, NULL, NULL, NULL, 7, 0, 0);
