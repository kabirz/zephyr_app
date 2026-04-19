#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <common.h>
#include <display.h>
#include <mod-can.h>
#include <lora.h>
#include <mod-gpio.h>

LOG_MODULE_REGISTER(power_gpio, LOG_LEVEL_INF);
#define USER_NODE DT_PATH(zephyr_user)
static const struct gpio_dt_spec charge_full = GPIO_DT_SPEC_GET(USER_NODE, chargefull_gpios);
static const struct gpio_dt_spec charging = GPIO_DT_SPEC_GET(USER_NODE, charging_gpios);
static const struct gpio_dt_spec power_button = GPIO_DT_SPEC_GET(USER_NODE, power_gpios);
static const struct gpio_dt_spec handle_button = GPIO_DT_SPEC_GET(USER_NODE, handlebt_gpios);
static const struct gpio_dt_spec link_switch = GPIO_DT_SPEC_GET(USER_NODE, linksw_gpios);

static const struct gpio_dt_spec can_power_gpio = GPIO_DT_SPEC_GET(USER_NODE, canpower_gpios);
static const struct gpio_dt_spec lora_power_gpio = GPIO_DT_SPEC_GET(USER_NODE, lorapower_gpios);
static const struct gpio_dt_spec dis_power_gpio = GPIO_DT_SPEC_GET(USER_NODE, dispower_gpios);
static const struct gpio_dt_spec handler_power_gpio = GPIO_DT_SPEC_GET(USER_NODE, handlerpower_gpios);

static struct gpio_callback power_button_cb_data;
static struct gpio_callback linksw_cb_data;

static struct k_work_delayable btn_display_work;
static struct k_work linksw_work;
static struct k_work_delayable sleep_work;

static void btn_display_work_handler(struct k_work *work)
{
	if (global_params.sleeping) {
		return;
	}
	if (gpio_pin_get_dt(&handle_button) == 0 && global_params.h_button) {
		global_params.h_button = 0;
		last_activity_time = k_uptime_get_32();
		mod_display_handler_button(global_params.h_button);

		if (global_params.connect_type == CAN_TYPE) {
			mod_can_send_handler_state(&global_params);
		} else {
			lora_send_telemetry(&global_params);
		}
	}
}

static void linksw_work_handler(struct k_work *work)
{
	global_params.connect_type = (global_params.connect_type == CAN_TYPE) ? LORA_TYPE : CAN_TYPE;
	if (global_params.connect_type == CAN_TYPE) {
		lora_power_enable(false);
		can_power_enable(true);
		k_event_clear(&global_params.event, LORA_EVENT);
		k_event_set(&global_params.event, CAN_EVENT);
		mod_display_can();
	} else {
		can_power_enable(false);
		lora_power_enable(true);
		k_event_clear(&global_params.event, CAN_EVENT);
		k_event_set(&global_params.event, LORA_EVENT);
		mod_display_lora(global_params.rssi);
	}
	LOG_INF("Link switch: %s", global_params.connect_type == CAN_TYPE ? "CAN" : "LoRa");
}

void can_power_enable(bool up)
{
	gpio_pin_set_dt(&can_power_gpio, up);
}

void lora_power_enable(bool up)
{
	gpio_pin_set_dt(&lora_power_gpio, up);
}

void dis_power_enable(bool up)
{
	gpio_pin_set_dt(&dis_power_gpio, up);
}

void handler_power_enable(bool up)
{
	gpio_pin_set_dt(&handler_power_gpio, up);
}
static void system_sleep(void)
{
	k_event_clear(&global_params.event, WAKE_EVENT);
	global_params.sleeping = true;
	can_power_enable(false);
	lora_power_enable(false);
	dis_power_enable(false);
	handler_power_enable(false);
	LOG_INF("system entering sleep");
}

static void system_wake(void)
{
	handler_power_enable(true);
	dis_power_enable(true);
	can_power_enable(true);
	lora_power_enable(true);
	k_msleep(200);
	mod_display_reinit();
	mod_display_all(&global_params);
	global_params.sleeping = false;
	last_activity_time = k_uptime_get_32();
	k_event_set(&global_params.event, WAKE_EVENT);
	LOG_INF("system woke up");
}

static void sleep_work_handler(struct k_work *work)
{
	if (gpio_pin_get_dt(&power_button) == 0) {
		if (global_params.sleeping) {
			system_wake();
		} else {
			system_sleep();
		}
	}
}

battery_status_t read_battery_status(void)
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
		k_work_reschedule(&sleep_work, K_MSEC(10));
	} else if (pins & BIT(handle_button.pin)) {
		if (global_params.sleeping) {
			return;
		}
		k_work_reschedule(&btn_display_work, K_MSEC(10));
	}
}

static void linksw_irq(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	if (global_params.sleeping) {
		return;
	}
	k_work_submit(&linksw_work);
}

static int power_init(void)
{
	int ret = 0;

	ret = gpio_pin_configure_dt(&can_power_gpio, GPIO_OUTPUT);
	if (ret < 0) {
		LOG_ERR("Failed to configure can power pin: %d", ret);
		return ret;
	}

	ret = gpio_pin_configure_dt(&lora_power_gpio, GPIO_OUTPUT);
	if (ret < 0) {
		LOG_ERR("Failed to configure lora power pin: %d", ret);
		return ret;
	}

	ret = gpio_pin_configure_dt(&dis_power_gpio, GPIO_OUTPUT);
	if (ret < 0) {
		LOG_ERR("Failed to configure display power pin: %d", ret);
		return ret;
	}

	ret = gpio_pin_configure_dt(&handler_power_gpio, GPIO_OUTPUT);
	if (ret < 0) {
		LOG_ERR("Failed to configure 5v power pin: %d", ret);
		return ret;
	}

	global_params.can_heart_time = CAN_HEART_TIME;
	global_params.connect_type = CAN_TYPE;
	k_event_init(&global_params.event);
	k_event_set(&global_params.event, CAN_EVENT);

	dis_power_enable(true);
	can_power_enable(true);
	handler_power_enable(true);

	return 0;
}

int gpio_init(void)
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

	k_work_init_delayable(&btn_display_work, btn_display_work_handler);
	k_work_init(&linksw_work, linksw_work_handler);
	k_work_init_delayable(&sleep_work, sleep_work_handler);

	LOG_INF("GPIO initialized successfully");
	LOG_INF("  PB12 (Charge Full): %s", gpio_pin_get_dt(&charge_full) ? "HIGH" : "LOW");
	LOG_INF("  PB13 (Charging):    %s", gpio_pin_get_dt(&charging) ? "HIGH" : "LOW");
	LOG_INF("  PB14 (Power Button): %s", gpio_pin_get_dt(&power_button) ? "HIGH" : "LOW");

	return 0;
}

SYS_INIT(power_init, PRE_KERNEL_2, 1);
