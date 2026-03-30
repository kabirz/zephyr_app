#include <zephyr/init.h>
#include <power.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(lora_serial, LOG_LEVEL_INF);

static const struct gpio_dt_spec lora_reset_pin = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), lorareset_gpios);
static const struct gpio_dt_spec lora_hostwake_pin = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), hostwake_gpios);
static K_MUTEX_DEFINE(hostwake_mutex);

int lora_stopclear(void);
#define USER_NODE DT_PATH(zephyr_user)
static const struct device *uart_dev = DEVICE_DT_GET(DT_NODELABEL(usart2));

struct rx_buf {
	uint32_t len;
	uint8_t data[128];
};
K_MSGQ_DEFINE(lora_serial_msgq, sizeof(struct rx_buf), 3, 4);
static bool hostwake_input;
/*
 * return : busy is true
 */
bool lora_get_hostwake_status(void)
{
	k_mutex_lock(&hostwake_mutex, K_FOREVER);
	if (hostwake_input == false) {
		gpio_pin_configure_dt(&lora_hostwake_pin, GPIO_INPUT|GPIO_PULL_UP);
		k_busy_wait(100);
		hostwake_input = true;
	}
	int val = gpio_pin_get_dt(&lora_hostwake_pin);
	k_mutex_unlock(&hostwake_mutex);
	return val == 1 ? true : false;
}

/*
 * send: send is true, idle is false
 */
void lora_set_hostwake_status(bool send)
{
	k_mutex_lock(&hostwake_mutex, K_FOREVER);
	if (hostwake_input == true) {
		gpio_pin_configure_dt(&lora_hostwake_pin, GPIO_OUTPUT);
		k_busy_wait(100);
		hostwake_input = false;
	}
	if (send) {
		gpio_pin_set_dt(&lora_hostwake_pin, 1);
		k_msleep(5);
	} else {
		gpio_pin_set_dt(&lora_hostwake_pin, 0);
		gpio_pin_configure_dt(&lora_hostwake_pin, GPIO_INPUT|GPIO_PULL_UP);
	}
	k_mutex_unlock(&hostwake_mutex);
}

static void lora_msg_process_thread(void)
{
	struct rx_buf buf;

	while (true) {
		if (k_msgq_get(&lora_serial_msgq, &buf, K_FOREVER) == 0) {
			LOG_HEXDUMP_INF(buf.data, buf.len, "Lora RX:");
		}
	}
}
K_THREAD_DEFINE(lora_msg, 1024, lora_msg_process_thread, NULL, NULL, NULL, 12, 0, 0);

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
static void uart_cb(const struct device *dev, void *user_data)
{
	static struct rx_buf buf;

	while (uart_irq_update(dev) && uart_irq_rx_ready(dev)) {
		buf.len += uart_fifo_read(dev, buf.data + buf.len, sizeof(buf.data) - buf.len);
		LOG_HEXDUMP_INF(buf.data, buf.len, "RX:");
		if (buf.len >= sizeof(buf.data)) {
			LOG_WRN("too more lora data");
			LOG_HEXDUMP_INF(buf.data, buf.len, "RX:");
			memset(&buf, 0, sizeof(buf));
		} else if (buf.len > 2) {
			if (buf.data[buf.len-1] == 0x0A && buf.data[buf.len-2] == 0x0D) {
				k_msgq_put(&lora_serial_msgq, &buf, K_NO_WAIT);
				memset(&buf, 0, sizeof(buf));
			}
		}
	}
}
#else
static void lora_uart_process_thread(void)
{
	static struct rx_buf buf = {0};
	int ret;

	while (true) {
		ret = uart_poll_in(uart_dev, buf.data + buf.len);
		if (ret == -1) {
			k_msleep(1);
			continue;
		}
		buf.len += 1;
		if (buf.len >= sizeof(buf.data)) {
			LOG_WRN("too more lora data");
			LOG_HEXDUMP_INF(buf.data, buf.len, "RX:");
			memset(&buf, 0, sizeof(buf));
		} else if (buf.len > 2) {
			if (buf.data[buf.len-1] == 0x0A && buf.data[buf.len-2] == 0x0D) {
				k_msgq_put(&lora_serial_msgq, &buf, K_NO_WAIT);
				memset(&buf, 0, sizeof(buf));
			}
		}
	}
}
K_THREAD_DEFINE(lora_uart_poll, 1024, lora_uart_process_thread, NULL, NULL, NULL, 13, 0, 0);
#endif

static bool serial_send(const uint8_t *data, size_t len, uint32_t event)
{
	if (lora_get_hostwake_status()) {
		// lora busy
		return false;
	} else {
		lora_set_hostwake_status(true);
	}
	for (size_t i = 0; i < len; i++) {
		uart_poll_out(uart_dev, data[i]);
	}

	LOG_HEXDUMP_INF(data, len, "TX:");
	lora_set_hostwake_status(false);
	return true;
}

static int lora_serial_init(void)
{
	int ret;
	ret = gpio_pin_configure_dt(&lora_reset_pin, GPIO_OUTPUT);
	if (ret < 0) {
		LOG_ERR("Failed to configure handle button pin: %d", ret);
		return ret;
	}
	lora_power_enable(true);
	gpio_pin_set_dt(&lora_reset_pin, 0);
	k_msleep(10);
	gpio_pin_set_dt(&lora_reset_pin, 1);
	k_msleep(10);
	gpio_pin_configure_dt(&lora_hostwake_pin, GPIO_INPUT|GPIO_PULL_UP);
	hostwake_input = true;
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
	uart_irq_callback_set(uart_dev, uart_cb);
	uart_irq_rx_enable(uart_dev);
#endif

	return 0;
}

SYS_INIT(lora_serial_init, APPLICATION, 10);
#ifdef CONFIG_SHELL
#include <zephyr/shell/shell.h>

static int cmd_send(const struct shell *ctx, size_t argc, char **argv)
{
	static char tx_buf[64];
	int len = snprintf(tx_buf, sizeof(tx_buf), "%s\r\n", argv[1]);

	if (serial_send(tx_buf, len, 0xf) == false) {
		shell_error(ctx, "lora is busy, wait and send agian!");
	}

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_serial_cmds,
			       SHELL_CMD_ARG(send, NULL,
					     "lora send\n"
					     "Usage: send <data>",
					     cmd_send, 1, 1),
			       SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(lora, &sub_serial_cmds, "lora commands", NULL);
#endif
