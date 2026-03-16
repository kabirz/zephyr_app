#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/pm.h>
#include <common.h>
LOG_MODULE_REGISTER(mod_power, LOG_LEVEL_INF);

static const struct gpio_dt_spec can_power_gpio = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), canpower_gpios);
static const struct gpio_dt_spec lora_power_gpio = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), lorapower_gpios);
static const struct gpio_dt_spec dis_power_gpio = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), dispower_gpios);
static const struct gpio_dt_spec p5_power_gpio = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), power5en_gpios);

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

void p5_power_enable(bool up)
{
	gpio_pin_set_dt(&p5_power_gpio, up);
}
static void notify_pm_state_entry(enum pm_state state)
{
	if (state == PM_STATE_SUSPEND_TO_IDLE) {
		p5_power_enable(true);
		dis_power_enable(true);
	} else {
		LOG_ERR("power pm error state");
	}
}

static void notify_pm_state_exit(enum pm_state state)
{

	if (state == PM_STATE_SUSPEND_TO_IDLE) {
		p5_power_enable(false);
		dis_power_enable(false);
	} else {
		LOG_ERR("power pm error state");
	}
}
static struct pm_notifier notifier = {
	.state_entry = notify_pm_state_entry,
	.state_exit = notify_pm_state_exit,
};

static int can_power_init(void)
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

	ret = gpio_pin_configure_dt(&p5_power_gpio, GPIO_OUTPUT);
	if (ret < 0) {
		LOG_ERR("Failed to configure 5v power pin: %d", ret);
		return ret;
	}

	pm_notifier_register(&notifier);
	return 0;
}
SYS_INIT(can_power_init, APPLICATION, 11);
