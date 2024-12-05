#include <zephyr/kernel.h>
#include <zephyr/app_version.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(modbus_main, CONFIG_MODBUS_APP_LOG_LEVEL);
static bool need_reboot;
static bool get_reboot_status(void)
{
	return need_reboot;
}
void set_reboot_status(bool val)
{
	need_reboot = true;
}

/* reboot if stack overflow */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *pEsf)
{
	if (reason == K_ERR_STACK_CHK_FAIL) {
		sys_reboot(SYS_REBOOT_WARM);
	}
}

int main(void)
{
	const struct gpio_dt_spec status_gpios = GPIO_DT_SPEC_GET(DT_ALIAS(mcuboot_led0), gpios);

	gpio_pin_configure_dt(&status_gpios, GPIO_OUTPUT_INACTIVE);
	LOG_INF("build time: %s-%s, board: %s, system clk: %dMHz, version: %s", __DATE__, __TIME__,
		CONFIG_BOARD_TARGET, CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / 1000000,
		APP_VERSION_STRING);
	need_reboot = false;
	while (1) {
		gpio_pin_set_dt(&status_gpios, 0);
		k_msleep(2700);
		gpio_pin_set_dt(&status_gpios, 1);
		k_msleep(300);
		if (get_reboot_status()) {
			LOG_INF("rebooting...");
			while (log_process() == true)
				;
			k_msleep(1000);
			sys_reboot(SYS_REBOOT_COLD);
		}
	}
	return 0;
}
