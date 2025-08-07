#include "zephyr/init.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>


static int board_init(void)
{
	const struct gpio_dt_spec reset_gpios = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), ethreset_gpios);

	gpio_pin_configure_dt(&reset_gpios, GPIO_OUTPUT_LOW);
	gpio_pin_set_dt(&reset_gpios, 0);
	k_msleep(1);
	gpio_pin_set_dt(&reset_gpios, 1);
	return 0;
}

SYS_INIT(board_init, POST_KERNEL, 1);
