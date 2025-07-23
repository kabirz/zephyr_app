#include "laser-can.h"
#include <zephyr/device.h>
#include <stdlib.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <laser-common.h>
LOG_MODULE_REGISTER(laser_serial, LOG_LEVEL_INF);

int laser_stopclear(void);
#define USER_NODE DT_PATH(zephyr_user)
static const struct device *uart_dev = DEVICE_DT_GET(DT_ALIAS(laser_serial));

#if DT_NODE_HAS_PROP(USER_NODE, rs485_tx_gpios)
static const struct gpio_dt_spec rs485tx_gpios = GPIO_DT_SPEC_GET(USER_NODE, rs485_tx_gpios);
#endif
struct rx_buf {
	uint32_t len;
	uint8_t data[128];
};
K_MSGQ_DEFINE(laser_serial_msgq, sizeof(struct rx_buf), 3, 4);
static uint8_t id;
static uint8_t tx_buf[256];
struct error_msg {
	uint16_t code;
	char *code_desc;
} error_msg_list [] = {
	{  0, "No Error"},
	{200, "sensor boot"},
	{203, "error command"},
	{210, "sensors not in tracking mode"},
	{211, "track time too short"},
	{212, "fisrt run stop/clear then run this command"},
	{220, "serail data error"},
};

static char *get_error_desc(uint16_t code)
{
	for (size_t i = 0; i < ARRAY_SIZE(error_msg_list); i++) {
		if (error_msg_list[i].code == code)
			return error_msg_list[i].code_desc;
	}
	return "Unkown error";
}

static void laser_msg_process_thread(void)
{
	struct rx_buf buf;

	while (true) {
		if (k_msgq_get(&laser_serial_msgq, &buf, K_FOREVER) == 0) {
			if (buf.data[0] != 'g' && buf.data[1] != id) continue;
			if (buf.data[2] == 'h') { // Distance
				buf.data[buf.len-2] = '\0';
				int32_t distance = strtol(buf.data+3, NULL, 10);
				LOG_INF("distance: %d", distance);
				if (!atomic_test_bit(&laser_status, LASER_WRITE_MODE) &&
					!atomic_test_bit(&laser_status, LASER_FW_UPDATE) &&
					atomic_test_bit(&laser_status, LASER_CON_MESURE) &&
					atomic_test_bit(&laser_status, LASER_DEVICE_STATUS)
				) {
					uint32_t can_data[2];
#if defined(CONFIG_BOARD_LASER_F103RET7)
					int32_t encode1, encode2;
					laser_get_encode_data(&encode1, &encode2);
					can_data[0] = ((uint32_t)encode1 & 0xFFFFF) << 12 | ((uint32_t)encode2 & 0xFFF00) >> 8;
					can_data[1] = (distance & 0xFFFFFF) | ((uint32_t)encode2 & 0xFF) << 24;
#else
					uint32_t encode1 = 0x1234, encode2 = 0x5678;
					can_data[0] = (encode1 & 0xFFFFF) << 12 | (encode2 & 0xFFF00) >> 8;
					can_data[1] = (distance & 0xFFFFFF) | (encode2 & 0xFF) << 24;
#endif
					cob_msg_send(can_data[0], can_data[1], 0x2E4);
				} else if (atomic_test_bit(&laser_status, LASER_FW_UPDATE)) {
					if (k_uptime_get() - latest_fw_up_times > 10000) {
						atomic_clear_bit(&laser_status, LASER_FW_UPDATE);
						LOG_INF("Cancel Firmware upgrade status due to timeout 10s");
					}
				}

			} else if (buf.data[2] == '@' && buf.data[3] == 'E') { // Error code
				static uint32_t device_old_status;
				static uint16_t old_err_code;
				uint32_t can_data[2] = {0};
				uint32_t device_status = atomic_test_bit(&laser_status, LASER_DEVICE_STATUS);
				buf.data[buf.len-2] = '\0';
				uint16_t err_code = strtol(buf.data+4, NULL, 10);
				LOG_ERR("laser error code: %s(%d)", get_error_desc(err_code), err_code);

				uint16_t tmp = err_code;
				for (size_t i = 0; i < 3; i++) {
					can_data[0] |= (tmp % 10  + '0') << 8;
					tmp /= 10;
				}
				can_data[1] = device_status;
				if (old_err_code != err_code) {
					cob_msg_send(can_data[0], can_data[1], 0x1E4);
					old_err_code = err_code;
				} else if ( device_status != device_old_status) {
					cob_msg_send(can_data[0], can_data[1], 0x1E4);
				}

				device_old_status = device_status;
			} else if (buf.data[2] == '?') { // stop/clear
				if (atomic_test_bit(&laser_status, LASER_ON))
					LOG_INF("laser on reply");
				else
					LOG_INF("laser stop/clear reply");
			}
		}
	}
}
K_THREAD_DEFINE(laser_msg, 2048, laser_msg_process_thread, NULL, NULL, NULL, 12, 0, 0);

static void uart_cb(const struct device *dev, void *user_data)
{
	static struct rx_buf buf;

	while (uart_irq_update(dev) && uart_irq_rx_ready(dev)) {
		buf.len += uart_fifo_read(dev, buf.data + buf.len, sizeof(buf.data) - buf.len);
		if (buf.len > sizeof(buf.data) - 4) {
			memset(&buf, 0, sizeof(buf));
		} else if (buf.len > 2) {
			if (buf.data[buf.len-1] == 0x0A && buf.data[buf.len-2] == 0x0D) {
				/* LOG_HEXDUMP_INF(buf.data, buf.len, "RX:"); */
				if (atomic_test_and_clear_bit(&laser_status, LASER_NEED_CLOSE)) {
					laser_stopclear();
				} else {
					k_msgq_put(&laser_serial_msgq, &buf, K_NO_WAIT);
				}
				memset(&buf, 0, sizeof(buf));
			}
		}
	}
}

static void serial_send(const uint8_t *data, size_t len)
{
#if DT_NODE_HAS_PROP(USER_NODE, rs485_tx_gpios)
	gpio_pin_set_dt(&rs485tx_gpios, 1);
#endif

	for (size_t i = 0; i < len; i++) {
		uart_poll_out(uart_dev, data[i]);
	}

#if DT_NODE_HAS_PROP(USER_NODE, rs485_tx_gpios)
	k_busy_wait(1000);
	gpio_pin_set_dt(&rs485tx_gpios, 0);
#endif
	LOG_HEXDUMP_INF(data, len, "TX:");
	k_sleep(K_MSEC(100));
}

int laser_stopclear(void)
{
	int len = snprintf(tx_buf, sizeof(tx_buf), "s%dc\r\n", id);
	serial_send(tx_buf, len);
	atomic_clear_bit(&laser_status, LASER_ON);
	atomic_clear_bit(&laser_status, LASER_CON_MESURE);

	return 0;
}

int laser_on(void)
{
	int len = snprintf(tx_buf, sizeof(tx_buf), "s%do\r\n", id);

	serial_send(tx_buf, len);

	atomic_set_bit(&laser_status, LASER_ON);

	return 0;
}

int laser_read_error(void)
{
	int len = snprintf(tx_buf, sizeof(tx_buf), "s%dre\r\n", id);
	serial_send(tx_buf, len);

	return 0;
}

int laser_clear_error(void)
{
	int len = snprintf(tx_buf, sizeof(tx_buf), "s%dce\r\n", id);
	serial_send(tx_buf, len);

	return 0;
}

int laser_con_measure(uint32_t val)
{

	laser_read_error();
	laser_clear_error();

	int len = snprintf(tx_buf, sizeof(tx_buf), "s%dh+%d\r\n", id, val);

	serial_send(tx_buf, len);
	atomic_set_bit(&laser_status, LASER_CON_MESURE);

	return 0;
}

int laser_setup(uint32_t val)
{
	// stop
	laser_stopclear();
	// on
	laser_on();

	laser_con_measure(val);

	atomic_set_bit(&laser_status, LASER_DEVICE_STATUS);
	return 0;
}

int laser_clear_err(void)
{
	int len = snprintf(tx_buf, sizeof(tx_buf), "s%dre\r\n", id);

	serial_send(tx_buf, len);

	return 0;
}

static int laser_serial_init(void)
{
	struct uart_config uart_cfg = {.baudrate = 19200,
				       .parity = UART_CFG_PARITY_EVEN,
				       .stop_bits = UART_CFG_STOP_BITS_1,
				       .data_bits = UART_CFG_DATA_BITS_7,
				       .flow_ctrl = UART_CFG_FLOW_CTRL_NONE};

	if (uart_configure(uart_dev, &uart_cfg)) {
		LOG_ERR("uart config error");
		return -1;
	}

#if DT_NODE_HAS_PROP(USER_NODE, rs485_tx_gpios)
	gpio_pin_configure_dt(&rs485tx_gpios, GPIO_OUTPUT_INACTIVE);
	gpio_pin_set_dt(&rs485tx_gpios, 0);
#endif
	/* laser_setup(); */
	uart_irq_callback_set(uart_dev, uart_cb);
	uart_irq_rx_enable(uart_dev);

	return 0;
}

SYS_INIT(laser_serial_init, APPLICATION, 10);

#ifdef CONFIG_SHELL
#include <zephyr/shell/shell.h>

static int cmd_laser_on(const struct shell *ctx, size_t argc, char **argv)
{
	return laser_on();
}

static int cmd_laser_mesure(const struct shell *ctx, size_t argc, char **argv)
{
	return laser_con_measure(100);
}

static int cmd_laser_write(const struct shell *ctx, size_t argc, char **argv)
{
	serial_send(argv[1], strlen(argv[1]));
	return 0;
}

static int cmd_laser_setup(const struct shell *ctx, size_t argc, char **argv)
{
	laser_setup(strtoul(argv[1], NULL, 0));
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_serial_cmds,
			       SHELL_CMD_ARG(on, NULL,
					     "laser on\n"
					     "Usage: on",
					     cmd_laser_on, 1, 0),
			       SHELL_CMD_ARG(mesure, NULL,
					     "laser mesure\n"
					     "Usage: mesure",
					     cmd_laser_mesure, 1, 0),
			       SHELL_CMD_ARG(send, NULL,
					     "serial send\n"
					     "Usage: send <strings>",
					     cmd_laser_write, 2, 0),
			       SHELL_CMD_ARG(setup, NULL,
					     "setup\n"
					     "Usage: setup <period>",
					     cmd_laser_setup, 2, 0),
			       SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(laser, &sub_serial_cmds, "laser commands", NULL);
#endif
