#include <fw_uart.h>
#include <fw_upgrade.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <string.h>

LOG_MODULE_REGISTER(fw_uart, LOG_LEVEL_INF);

static const struct device *uart_dev = DEVICE_DT_GET(DT_NODELABEL(usart1));

typedef enum {
	UART_STATE_IDLE,
	UART_STATE_TYPE,
	UART_STATE_LEN_H,
	UART_STATE_LEN_L,
	UART_STATE_DATA,
	UART_STATE_CRC_H,
	UART_STATE_CRC_L,
} uart_rx_state_t;

static uart_rx_state_t rx_state = UART_STATE_IDLE;
static uint8_t rx_type;
static uint8_t rx_len;
static uint8_t rx_index;
static uint8_t rx_buffer[8];
static uint16_t rx_crc;

static struct can_frame uart_frame;

typedef struct {
	uint8_t type;
	uint8_t data[8];
	uint8_t len;
} fw_uart_frame_t;

K_MSGQ_DEFINE(uart_msgq, sizeof(fw_uart_frame_t), 8, 4);

static uint16_t crc16_ccitt(const uint8_t *data, uint16_t len)
{
	uint16_t crc = 0xFFFF;

	for (uint16_t i = 0; i < len; i++) {
		crc ^= (uint16_t)data[i];
		for (uint8_t j = 0; j < 8; j++) {
			if (crc & 0x0001) {
				crc = (crc >> 1) ^ 0xA001;
			} else {
				crc >>= 1;
			}
		}
	}
	return crc;
}

static void uart_send_frame(uint8_t type, uint8_t *data, uint8_t len)
{
	uint8_t frame[16];
	uint16_t index = 0;

	frame[index++] = UART_FRAME_HEAD;
	frame[index++] = type;
	frame[index++] = 0;
	frame[index++] = len;
	memcpy(&frame[index], data, len);
	index += len;
	uint16_t crc = crc16_ccitt(data, len);
	frame[index++] = (crc >> 8) & 0xFF;
	frame[index++] = crc & 0xFF;
	frame[index++] = UART_FRAME_TAIL;

	for (uint16_t i = 0; i < index; i++) {
		uart_poll_out(uart_dev, frame[i]);
	}
}

static void fw_uart_send_response(uint32_t code, uint32_t value)
{
	uint8_t data[8];
	memcpy(&data[0], &code, 4);
	memcpy(&data[4], &value, 4);
	uart_send_frame(UART_FRAME_CMD, data, 8);
}

static void uart_isr(const struct device *dev, void *user_data)
{
	while (uart_irq_update(dev) && uart_irq_rx_ready(dev)) {
		uint8_t byte;
		uart_fifo_read(dev, &byte, 1);

		switch (rx_state) {
		case UART_STATE_IDLE:
			if (byte == UART_FRAME_HEAD) {
				rx_state = UART_STATE_TYPE;
			}
			break;
		case UART_STATE_TYPE:
			if (byte == UART_FRAME_CMD || byte == UART_FRAME_DATA) {
				rx_type = byte;
				rx_index = 0;
				rx_state = UART_STATE_LEN_H;
			} else {
				rx_state = UART_STATE_IDLE;
			}
			break;
		case UART_STATE_LEN_H:
			rx_len = byte << 8;
			rx_state = UART_STATE_LEN_L;
			break;
		case UART_STATE_LEN_L:
			rx_len |= byte;
			if (rx_len <= 8) {
				rx_state = (rx_len > 0) ? UART_STATE_DATA : UART_STATE_CRC_H;
			} else {
				rx_state = UART_STATE_IDLE;
			}
			break;
		case UART_STATE_DATA:
			rx_buffer[rx_index++] = byte;
			if (rx_index >= rx_len) {
				rx_state = UART_STATE_CRC_H;
			}
			break;
		case UART_STATE_CRC_H:
			rx_crc = byte << 8;
			rx_state = UART_STATE_CRC_L;
			break;
		case UART_STATE_CRC_L:
			rx_crc |= byte;
			if (crc16_ccitt(rx_buffer, rx_len) == rx_crc) {
				fw_uart_frame_t frame;
				frame.type = rx_type;
				frame.len = rx_len;
				memcpy(frame.data, rx_buffer, rx_len);
				k_msgq_put(&uart_msgq, &frame, K_NO_WAIT);
			}
			rx_state = UART_STATE_IDLE;
			break;
		default:
			rx_state = UART_STATE_IDLE;
			break;
		}
	}
}

static void fw_uart_process_thread(void)
{
	fw_uart_frame_t frame;

	uart_irq_rx_disable(uart_dev);
	uart_irq_callback_user_data_set(uart_dev, uart_isr, NULL);
	uart_irq_rx_enable(uart_dev);

	LOG_INF("UART transport ready");

	while (true) {
		if (k_msgq_get(&uart_msgq, &frame, K_FOREVER) == 0) {
			fw_set_response_cb(fw_uart_send_response);
			memset(&uart_frame, 0, sizeof(uart_frame));

			if (frame.type == UART_FRAME_CMD) {
				LOG_INF("UART CMD received");
				uart_frame.id = CAN_ID_PLATFORM_RX;
			} else if (frame.type == UART_FRAME_DATA) {
				uart_frame.id = CAN_ID_FW_DATA_RX;
			}

			uart_frame.dlc = can_bytes_to_dlc(frame.len);
			memcpy(uart_frame.data, frame.data, frame.len);
			fw_update(&uart_frame);
		}
	}
}

K_THREAD_DEFINE(fw_uart_rx, 2048, fw_uart_process_thread, NULL, NULL, NULL, 9, 0, 0);
