#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/util.h>
#include <zephyr/kernel.h>

#define USER_NODE DT_PATH(zephyr_user)
static const struct device *uart_dev = DEVICE_DT_GET(DT_NODELABEL(usart2));
static const struct gpio_dt_spec rs485tx_gpios = GPIO_DT_SPEC_GET(USER_NODE, rs485_tx_gpios);
static uint8_t rx_buf[128];
static uint8_t hex_buf[128];

static void uart_cb(const struct device *dev, void *user_data)
{
	while (uart_irq_update(dev) && uart_irq_rx_ready(dev)) {
		int len = uart_fifo_read(dev, rx_buf, sizeof(rx_buf));
		if (len > 0) {
			bin2hex(rx_buf, len, hex_buf, sizeof(hex_buf));
			printk("RX: %s\n", hex_buf);
		}
	}
}

static void rs485_send(const uint8_t *data, size_t len)
{
	gpio_pin_set_dt(&rs485tx_gpios, 1);
	k_busy_wait(50);

	for (size_t i = 0; i < len; i++) {
		uart_poll_out(uart_dev, data[i]);
	}

	k_busy_wait(50);
	gpio_pin_set_dt(&rs485tx_gpios, 0);
}

int laster_stopclear(void)
{
	uint8_t data[] = {0x73, 0x30, 0x63, 0x0D, 0x0A};

	rs485_send(data, sizeof(data));

	return 0;
}

int laster_on(void)
{
	uint8_t data[] = {0x73, 0x30, 0x63, 0x0D, 0x0A};

	rs485_send(data, sizeof(data));

	return 0;
}

int laster_con_measure(void)
{
	uint8_t data[] = {0x73, 0x30, 0x68, 0x0D, 0x0A};

	rs485_send(data, sizeof(data));

	return 0;
}

static int rs485_init(void)
{

	struct uart_config uart_cfg = {.baudrate = 19200,
				       .parity = UART_CFG_PARITY_EVEN,
				       .stop_bits = UART_CFG_STOP_BITS_1,
				       .data_bits = UART_CFG_DATA_BITS_7,
				       .flow_ctrl = UART_CFG_FLOW_CTRL_NONE};

	uart_configure(uart_dev, &uart_cfg);

	gpio_pin_configure_dt(&rs485tx_gpios, GPIO_OUTPUT_INACTIVE);

	uart_irq_callback_set(uart_dev, uart_cb);
	uart_irq_rx_enable(uart_dev);

	return 0;
}

void rs_demo(void)
{
	while (true) {
		rs485_send("Hello RS485", 8);
		k_sleep(K_MSEC(1));
	}
}

SYS_INIT(rs485_init, APPLICATION, 10);

#ifdef CONFIG_SHELL
#include <zephyr/shell/shell.h>
static int cmd_rs485_write(const struct shell *ctx, size_t argc, char **argv)
{
	rs485_send(argv[2], strlen(argv[2]));
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_rs485_cmds,
			       SHELL_CMD_ARG(send, NULL,
					     "rs485 send\n"
					     "Usage: send <strings>",
					     cmd_rs485_write, 1, 0),
			       SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(rs485, &sub_rs485_cmds, "rs485 commands", NULL);
#endif
