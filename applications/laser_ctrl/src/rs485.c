#include "laser-can.h"
#include <zephyr/device.h>
#include <stdlib.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <laser-common.h>
LOG_MODULE_REGISTER(laser_rs485, LOG_LEVEL_INF);

int laser_stopclear(void);
#define USER_NODE DT_PATH(zephyr_user)
#if defined(CONFIG_BOARD_MONV_F407VET6)
static const struct device *uart_dev = DEVICE_DT_GET(DT_NODELABEL(usart3));
#elif defined(CONFIG_BOARD_LASER_F103RET7)
static const struct device *uart_dev = DEVICE_DT_GET(DT_NODELABEL(usart2));
#endif

#if DT_NODE_HAS_PROP(USER_NODE, rs485_tx_gpios)
static const struct gpio_dt_spec rs485tx_gpios = GPIO_DT_SPEC_GET(USER_NODE, rs485_tx_gpios);
#endif
struct rx_buf {
	uint32_t len;
	uint8_t data[64];
};
K_MSGQ_DEFINE(laser_rs485_msgq, sizeof(struct rx_buf), 3, 4);
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
		if (k_msgq_get(&laser_rs485_msgq, &buf, K_FOREVER) == 0) {
			if (buf.data[0] != 'g' && buf.data[1] != id) return;
			if (buf.data[2] == 'h') { // Distance
				buf.data[buf.len-2] = '\0';
				int32_t distance = strtol(buf.data+3, NULL, 0);
				LOG_DBG("distance: %d", distance);
				if (!atomic_test_bit(&laser_status, LASER_WRITE_MODE) &&
					!atomic_test_bit(&laser_status, LASER_FW_UPDATE) &&
					atomic_test_bit(&laser_status, LASER_CON_MESURE)
				) {
					struct can_frame frame = {
						.id = 0x2E4,
						.dlc = can_bytes_to_dlc(8),
					};
#if defined(CONFIG_BOARD_LASER_F103RET7)
					int32_t encode1, encode2;
					laser_get_encode_data(&encode1, &encode2);
					frame.data_32[0] = (encode1 & 0xFFFFF) << 12 | (encode2 & 0xFFF00) >> 8;
					frame.data_32[1] = (distance & 0xFFFFFF) << 12 | (encode2 & 0xFF) << 24;
#endif
					laser_can_send(&frame);
				} else if (atomic_test_bit(&laser_status, LASER_FW_UPDATE)) {
					if (k_uptime_get() - latest_fw_up_times > 10000) {
						atomic_clear_bit(&laser_status, LASER_FW_UPDATE);
						LOG_INF("Cancel Firmware upgrade status due to timeout 10s");
					}
				}

			} else if (buf.data[2] == '@' && buf.data[3] == 'E') { // Error code
				buf.data[buf.len-2] = '\0';
				uint16_t err_code = strtol(buf.data+4, NULL, 0);
				LOG_ERR("laser error code: %s(%d)", get_error_desc(err_code), err_code);
			} else if (buf.data[2] == '?') { // stop/clear
				LOG_INF("stop/clear reply");
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
		if (buf.len > 2) {
			if (buf.data[buf.len-1] == 0x0A && buf.data[buf.len-2] == 0x0D) {
				if (atomic_test_and_clear_bit(&laser_status, LASER_NEED_CLOSE)) {
					laser_stopclear();
				} else {
				k_msgq_put(&laser_rs485_msgq, &buf, K_NO_WAIT);
				memset(&buf, 0, sizeof(buf));
				}
			}
		}
	}
}

static void rs485_send(const uint8_t *data, size_t len)
{
#if DT_NODE_HAS_PROP(USER_NODE, rs485_tx_gpios)
	gpio_pin_set_dt(&rs485tx_gpios, 1);
	k_busy_wait(30);
#endif

	for (size_t i = 0; i < len; i++) {
		uart_poll_out(uart_dev, data[i]);
	}

#if DT_NODE_HAS_PROP(USER_NODE, rs485_tx_gpios)
	k_busy_wait(30);
	gpio_pin_set_dt(&rs485tx_gpios, 0);
#endif
}

int laser_stopclear(void)
{
	int len = snprintf(tx_buf, sizeof(tx_buf), "s%dc\r\n", id);
	rs485_send(tx_buf, len);
	atomic_clear_bit(&laser_status, LASER_ON);
	atomic_clear_bit(&laser_status, LASER_CON_MESURE);

	return 0;
}

int laser_on(void)
{
	int len = snprintf(tx_buf, sizeof(tx_buf), "s%do+1\r\n", id);

	rs485_send(tx_buf, len);

	atomic_set_bit(&laser_status, LASER_ON);

	return 0;
}

int laser_con_measure(uint32_t val)
{
	int len = snprintf(tx_buf, sizeof(tx_buf), "s%dh+%d\r\n", id, val);

	rs485_send(tx_buf, len);
	atomic_set_bit(&laser_status, LASER_CON_MESURE);

	return 0;
}

int laser_clear_err(void)
{
	int len = snprintf(tx_buf, sizeof(tx_buf), "s%dre\r\n", id);

	rs485_send(tx_buf, len);

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

#if DT_NODE_HAS_PROP(USER_NODE, rs485_tx_gpios)
	gpio_pin_configure_dt(&rs485tx_gpios, GPIO_OUTPUT_INACTIVE);
	gpio_pin_set_dt(&rs485tx_gpios, 0);
#endif

	uart_irq_callback_set(uart_dev, uart_cb);
	uart_irq_rx_enable(uart_dev);

	return 0;
}

SYS_INIT(rs485_init, APPLICATION, 10);

#ifdef CONFIG_SHELL
#include <zephyr/shell/shell.h>
static int cmd_rs485_write(const struct shell *ctx, size_t argc, char **argv)
{
	rs485_send(argv[1], strlen(argv[1]));
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_rs485_cmds,
			       SHELL_CMD_ARG(send, NULL,
					     "rs485 send\n"
					     "Usage: send <strings>",
					     cmd_rs485_write, 2, 0),
			       SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(rs485, &sub_rs485_cmds, "rs485 commands", NULL);
#endif
