#include <zephyr/kernel.h>
#include <mod-can.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(mod_can, LOG_LEVEL_INF);

static const struct device *can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));
CAN_MSGQ_DEFINE(mod_can_msgq, 8);

int cob_msg_send(uint32_t data1, uint32_t data2, uint32_t id)
{
	struct can_frame frame = {
		.data_32 = {
			sys_cpu_to_be32(data1),
			sys_cpu_to_be32(data2),
		},
		.id = id,
		.dlc = can_bytes_to_dlc(8),
	};


	return mod_can_send(&frame);
}

static void mod_canrx_msg_handler(struct can_frame *frame)
{
	switch (frame->id) {
	case PLATFORM_RX:
	case FW_DATA_RX:
		fw_update(frame);
		break;
	default:
		LOG_ERR("can frame id (0x%x) is not support", frame->id);
	}
}

static void mod_cantx_callback(const struct device *dev, int error, void *user_data)
{
	uint32_t count = *(uint32_t *)user_data;
	if (error == 0) {
		LOG_DBG("CAN frame #%u successfully sent", count);
	} else {
		LOG_ERR("failed to send CAN frame #%u (err %d)", count, error);
	}
}

int mod_can_send(struct can_frame *frame)
{
	static uint32_t frame_count = 0;

	frame_count++;

	return can_send(can_dev, frame, K_MSEC(100), mod_cantx_callback, &frame_count);
}

bool check_can_device_ready(void)
{
	return DEVICE_API_IS(can, can_dev) && device_is_ready(can_dev);
}

int mod_can_init(void)
{
	int err = -1;
	uint8_t can_check_time = 0;

	while (can_check_time++ < 10) {
		if (check_can_device_ready()) break;
	}
	if (can_check_time >= 10) {
		LOG_ERR("can device is not ready");
		goto end;
	}

	if ((err = can_set_bitrate(can_dev, 250000)) != 0) {
		LOG_ERR("failed to set bitrate (err %d)", err);
		goto end;
	}

	if ((err = can_start(can_dev)) != 0) {
		LOG_ERR("failed to start CAN controller (err %d)", err);
		goto end;
	}

	struct can_filter filter = {
		.mask = CAN_STD_ID_MASK
	};


	filter.id = PLATFORM_RX;
	can_add_rx_filter_msgq(can_dev, &mod_can_msgq, &filter);

	filter.id = FW_DATA_RX;
	can_add_rx_filter_msgq(can_dev, &mod_can_msgq, &filter);
end:
	return err;
}

void mod_can_process_thread(void)
{
	struct can_frame frame;

	if (mod_can_init() != 0) {
		LOG_ERR("can init failed");
		return;
	}


	while (true) {
		if (k_msgq_get(&mod_can_msgq, &frame, K_FOREVER) == 0)
			mod_canrx_msg_handler(&frame);
	}
}

K_THREAD_DEFINE(mod_can, 2048, mod_can_process_thread, NULL, NULL, NULL, 11, 0, 0);
