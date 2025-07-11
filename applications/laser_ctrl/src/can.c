#include <zephyr/kernel.h>
#include <laser-can.h>
#include <laser-rs485.h>
#include <laser-flash.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(laser_can, LOG_LEVEL_DBG);

static const struct device *can_dev = DEVICE_DT_GET(DT_NODELABEL(can1));
CAN_MSGQ_DEFINE(laser_can_msgq, 5);
static struct k_work_delayable laser_delayed_work;
uint64_t latest_fw_up_times;

static int cob_msg_send(uint32_t data1, uint32_t data2, uint32_t id)
{
	struct can_frame frame = {
		.data_32 = {
			sys_cpu_to_be32(data1),
			sys_cpu_to_be32(data2),
		},
		.id = id,
		.dlc = can_bytes_to_dlc(8),
	};


	return laser_can_send(&frame);
}

static int laser_enable(bool enable)
{
	LOG_DBG("enable: %d", enable);
	if (enable) {
		laser_on();
	} else {
		if (atomic_test_bit(&laser_status, LASER_ON) &&
				atomic_test_bit(&laser_status, LASER_CON_MESURE)) {
			atomic_set_bit(&laser_status, LASER_NEED_CLOSE);
			k_work_schedule(&laser_delayed_work, K_SECONDS(2));
		} else {
			laser_stopclear();
		}
	}
	cob_msg_send(0x60416200, enable, COB_ID1_TX);
	return 0;
}

static int laser_get_collectperiod(void)
{
	uint32_t val = 0;

	// TODO
	LOG_DBG("val: 0x%x", val);

	cob_msg_send(0x60426200, val, COB_ID1_TX);
	return 0;
}

static int laser_set_collectperiod(uint32_t val)
{
	laser_con_measure(val);
	LOG_DBG("val: 0x%x", val);
	cob_msg_send(0x60426200, 1, COB_ID1_TX);
	return 0;
}

static int laser_mem_writemode(void)
{
	laser_flash_write_mode();
	LOG_DBG("mem write mode");
	uint32_t mode = atomic_test_bit(&laser_status, LASER_WRITE_MODE) ? 0x10 : 0x1;
	cob_msg_send(mode, 0x22000000, COB_ID1_TX);
	return 0;
}

static int laser_mem_readmode(void)
{
	laser_flash_read_mode();
	LOG_DBG("mem read mode");
	cob_msg_send(0x1, 0x40000000, COB_ID1_TX);
	return 0;
}

static int laser_mem_writedata(uint16_t address, uint32_t val)
{
	int ret = laser_flash_write(address, val);

	LOG_DBG("address: %d, val: 0x%x", address, val);

	if (ret == 0) {
		cob_msg_send(0x1, val, COB_ID2_TX);
	} else {
		cob_msg_send(0x12, ret, COB_ID2_TX);
	}
	return 0;
}

static int laser_mem_readdata(uint16_t address)
{
	uint32_t val;
	int ret = laser_flash_read(address, &val);
	uint32_t mode = atomic_test_bit(&laser_status, LASER_WRITE_MODE) ? 0x10 : 0x1;

	LOG_DBG("address: %d, val: 0x%x", address, val);
	if (ret == 0) {
		cob_msg_send(mode, val, COB_ID2_TX);
	} else {
		cob_msg_send(0x2, ret, COB_ID2_TX);
	}
	return 0;
}

static void laser_canrx_msg_handler(struct can_frame *frame)
{
	switch (frame->id) {
		case COB_ID1_RX : {
			if (frame->data[0] == 0x22) {
				if (frame->data[1] == 0x0 && frame->data[2] == 0x0) {
					laser_mem_writemode();
				} else if (frame->data[1] == 0x41 && frame->data[2] == 0x62) {
					if(sys_be32_to_cpu(frame->data_32[1]))
						laser_enable(true);
					else
						laser_enable(false);
				} else if (frame->data[1] == 0x42 && frame->data[2] == 0x62) {
					laser_set_collectperiod(sys_be32_to_cpu(frame->data_32[1]));
				}
			} else if (frame->data[0] == 0x40) {
				if (frame->data[1] == 0x0 && frame->data[2] == 0x0) {
					laser_mem_readmode();
				} else if (frame->data[1] == 0x42 && frame->data[2] == 0x62) {
					laser_get_collectperiod();
				}
			}
		}
			break;
		case COB_ID2_RX : {
			if (frame->data[0] == 0x22) {
				uint32_t address = frame->data[1] + frame->data[2] * 10;
				laser_mem_writedata(address, sys_be32_to_cpu(frame->data_32[1]));
			} else if (frame->data[0] == 0x40) {
				uint32_t address = frame->data[1] + frame->data[2] * 10;
				laser_mem_readdata(address);
			}
		}
		case PLATFORM_RX:
		case FW_DATA_RX:
			atomic_set_bit(&laser_status, LASER_FW_UPDATE);
			latest_fw_up_times = k_uptime_get();
			fw_update(frame);
			break;
		default:
			LOG_ERR("can frame id (0x%x) is not support", frame->id);
	}
}

static void laser_cantx_callback(const struct device *dev, int error, void *user_data)
{
	uint32_t count = *(uint32_t *)user_data;
	if (error == 0) {
		if (!atomic_test_bit(&laser_status, LASER_FW_UPDATE))
			LOG_DBG("CAN frame #%u successfully sent", count);
	} else {
		LOG_ERR("failed to send CAN frame #%u (err %d)", count, error);
	}
}

static void laser_stop_work_handler(struct k_work *work)
{
	if (atomic_test_and_clear_bit(&laser_status, LASER_NEED_CLOSE)) {
		laser_stopclear();
	}
}

int laser_can_send(struct can_frame *frame)
{
	static uint32_t frame_count = 0;

	frame_count++;

	return can_send(can_dev, frame, K_MSEC(100), laser_cantx_callback, &frame_count);
}

bool check_can_device_ready(void)
{
	return DEVICE_API_IS(can, can_dev) && device_is_ready(can_dev);
}

int laser_can_init(void)
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

	laser_flash_read_mode();

	struct can_filter filter = {
		.mask = CAN_STD_ID_MASK
	};

	filter.id = COB_ID1_RX;
	can_add_rx_filter_msgq(can_dev, &laser_can_msgq, &filter);

	filter.id = COB_ID2_RX;
	can_add_rx_filter_msgq(can_dev, &laser_can_msgq, &filter);

	filter.id = PLATFORM_RX;
	can_add_rx_filter_msgq(can_dev, &laser_can_msgq, &filter);

	filter.id = FW_DATA_RX;
	can_add_rx_filter_msgq(can_dev, &laser_can_msgq, &filter);
end:
	return err;
}

void laser_can_process_thread(void)
{
	struct can_frame frame;

	if (laser_can_init() != 0) {
		LOG_ERR("can init failed");
		return;
	}

	k_work_init_delayable(&laser_delayed_work, laser_stop_work_handler);
	k_work_schedule(&laser_delayed_work, K_SECONDS(2));

	while (true) {
		if (k_msgq_get(&laser_can_msgq, &frame, K_FOREVER) == 0)
			laser_canrx_msg_handler(&frame);
	}
}

K_THREAD_DEFINE(laser_can, 1024, laser_can_process_thread, NULL, NULL, NULL, 12, 0, 0);

