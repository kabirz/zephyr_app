#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/app_version.h>

#include <fw_upgrade.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

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
		LOG_INF("Key1 pressed, enter upgrade mode");
		return true;
	}
#endif

	if (CheckUpgradeFlag()) {
		LOG_INF("Upgrade flag detected, clearing flag...");
		ClearUpgradeFlag();
		return true;
	}

	return false;
}

int main(void)
{
	LOG_INF("========================================");
	LOG_INF("  STM32 Bootloader %s", APP_VERSION_STRING);
	LOG_INF("========================================");
	LOG_INF("Flash: 0x%08x - 0x%08x", FLASH_APP_START_ADDR, FLASH_APP_END_ADDR);
	LOG_INF("App Start: 0x%08x", APP_START_ADDR);

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
		LOG_INF("App firmware valid, jumping to 0x%08x", APP_START_ADDR);
		JumpToApp(APP_START_ADDR);
	} else {
		LOG_INF("App firmware invalid!");
#if DT_NODE_EXISTS(DT_PATH(leds, led_1))
		if (gpio_is_ready_dt(&led2)) {
			gpio_pin_set_dt(&led2, 1);
		}
#endif
		LOG_INF("Staying in bootloader mode");
	}

	while (1) {
		k_sleep(K_FOREVER);
	}

	return 0;
}
