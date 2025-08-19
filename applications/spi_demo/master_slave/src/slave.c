#include "zephyr/init.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(slave_callback);

static const struct device *spi_dev = DEVICE_DT_GET(DT_NODELABEL(spi2));
static const struct device *cs_dev = DEVICE_DT_GET(DT_NODELABEL(gpioa)); // PA4
#define INT_PIN 4

const struct spi_config config = {
	.operation = SPI_OP_MODE_SLAVE,
};

static struct gpio_callback int_gpio_cb;

static uint8_t rx_buffer[128];
const struct spi_buf rx_buffers = {.buf = rx_buffer, .len = sizeof(rx_buffer)};
const struct spi_buf_set rx_buf_set = {.buffers = &rx_buffers, .count = 1};
static K_SEM_DEFINE(slave_sem, 1, 1);

void slave_proccess(void)
{
	k_sem_take(&slave_sem, K_FOREVER);
	k_busy_wait(10);
	if (gpio_pin_get(cs_dev, INT_PIN) != 0) return;

	int ret = spi_read(spi_dev, &config, &rx_buf_set);
	if (ret < 0) {
		LOG_ERR("slave spi read error!");
		return;
	}
	LOG_HEXDUMP_INF(rx_buffer, ret, "RX:");
}

static void cs_isr_handler(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	k_sem_give(&slave_sem);
}

static int slave_init(void)
{
	int r;
	r = gpio_pin_configure(cs_dev, INT_PIN, GPIO_INPUT);
	if (r < 0) {
		LOG_ERR("Could not configure interrupt GPIO pin");
		return r;
	}

	r = gpio_pin_interrupt_configure(cs_dev, INT_PIN, GPIO_INT_EDGE_TO_INACTIVE);
	if (r < 0) {
		LOG_ERR("Could not configure interrupt GPIO interrupt.");
		return r;
	}

	gpio_init_callback(&int_gpio_cb, cs_isr_handler, BIT(INT_PIN));

	r = gpio_add_callback(cs_dev, &int_gpio_cb);
	if (r < 0) {
		LOG_ERR("Could not set gpio callback");
		return r;
	}

	return 0;
}

SYS_INIT(slave_init, APPLICATION, 10);
