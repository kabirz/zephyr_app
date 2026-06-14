#include <fw_can.h>
#include <fw_upgrade.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/can.h>

static const struct device *can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));
CAN_MSGQ_DEFINE(fw_can_msgq, 8);

static struct can_filter filter_cmd = {
	.id = CAN_ID_PLATFORM_RX,
	.mask = CAN_STD_ID_MASK,
	.flags = 0,
};

static struct can_filter filter_data = {
	.id = CAN_ID_FW_DATA_RX,
	.mask = CAN_STD_ID_MASK,
	.flags = 0,
};

void fw_can_send_response(uint32_t code, uint32_t value)
{
	struct can_frame frame = {
		.id = CAN_ID_PLATFORM_TX,
		.data_32 = {code, value},
		.dlc = can_bytes_to_dlc(8),
	};

	can_send(can_dev, &frame, K_MSEC(100), NULL, NULL);
}

static void fw_canrx_msg_handler(struct can_frame *frame)
{
	fw_set_response_cb(fw_can_send_response);
	switch (frame->id) {
	case CAN_ID_PLATFORM_RX:
	case CAN_ID_FW_DATA_RX:
		fw_update(frame);
		break;
	default:
		break;
	}
}

static void fw_can_process_thread(void)
{
	struct can_frame frame;

	if (can_set_bitrate(can_dev, 250000) != 0) {
		return;
	}

	if (can_start(can_dev) != 0) {
		return;
	}

	if (can_add_rx_filter_msgq(can_dev, &fw_can_msgq, &filter_cmd) < 0) {
		return;
	}

	if (can_add_rx_filter_msgq(can_dev, &fw_can_msgq, &filter_data) < 0) {
		return;
	}

	while (true) {
		if (k_msgq_get(&fw_can_msgq, &frame, K_FOREVER) == 0) {
			fw_canrx_msg_handler(&frame);
		}
	}
}

K_THREAD_DEFINE(fw_can_rx, 2048, fw_can_process_thread, NULL, NULL, NULL, 8, 0, 0);
