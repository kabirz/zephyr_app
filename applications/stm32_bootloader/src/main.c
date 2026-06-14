#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/app_version.h>

#include <fw_upgrade.h>

#if DT_NODE_EXISTS(DT_PATH(gpio_keys, button_0))
static const struct gpio_dt_spec key1 = GPIO_DT_SPEC_GET(DT_PATH(gpio_keys, button_0), gpios);
#endif

#if DT_NODE_EXISTS(DT_PATH(leds, led_0))
static const struct gpio_dt_spec led1 = GPIO_DT_SPEC_GET(DT_PATH(leds, led_0), gpios);
#endif

#if DT_NODE_EXISTS(DT_PATH(leds, led_1))
static const struct gpio_dt_spec led2 = GPIO_DT_SPEC_GET(DT_PATH(leds, led_1), gpios);
#endif

static bool enter_upgrade_mode(void)
{
#if DT_NODE_EXISTS(DT_PATH(gpio_keys, button_0))
	if (gpio_is_ready_dt(&key1)) {
		gpio_pin_configure_dt(&key1, GPIO_INPUT);
	}

	if (gpio_pin_get_dt(&key1) == 0) {
		return true;
	}
#endif

	if (CheckUpgradeFlag()) {
		ClearUpgradeFlag();
		return true;
	}

	return false;
}

int main(void)
{
#if DT_NODE_EXISTS(DT_PATH(leds, led_0))
	if (gpio_is_ready_dt(&led1)) {
		gpio_pin_configure_dt(&led1, GPIO_OUTPUT_INACTIVE);
	}
#endif

#if DT_NODE_EXISTS(DT_PATH(leds, led_1))
	if (gpio_is_ready_dt(&led2)) {
		gpio_pin_configure_dt(&led2, GPIO_OUTPUT_INACTIVE);
	}
#endif

	if (enter_upgrade_mode()) {
#if DT_NODE_EXISTS(DT_PATH(leds, led_0))
		if (gpio_is_ready_dt(&led1)) {
			gpio_pin_set_dt(&led1, 1);
		}
#endif
	} else if (VerifyAppFirmware()) {
		JumpToApp(APP_START_ADDR);
	} else {
#if DT_NODE_EXISTS(DT_PATH(leds, led_1))
		if (gpio_is_ready_dt(&led2)) {
			gpio_pin_set_dt(&led2, 1);
		}
#endif
	}

	while (1) {
		k_sleep(K_FOREVER);
	}

	return 0;
}
