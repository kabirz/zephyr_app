#include <zephyr/kernel.h>
#include <mod-can.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
#include <common.h>
LOG_MODULE_REGISTER(mod_can, LOG_LEVEL_INF);

static const struct device *can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));
CAN_MSGQ_DEFINE(mod_can_msgq, 8);

static K_SEM_DEFINE(heart_wake_sem, 1, 1);
static atomic_t heart_send_success = ATOMIC_INIT(0);

static void mod_canrx_msg_handler(struct can_frame *frame)
{
	k_sem_give(&heart_wake_sem);

	switch (frame->id) {
	case PLATFORM_RX:
	case FW_DATA_RX:
		fw_update(frame);
		break;
	default:
		LOG_ERR("can frame id (0x%x) is not support", frame->id);
	}
}

static void heart_tx_callback(const struct device *dev, int error, void *user_data)
{
	if (error == 0) {
		atomic_set(&heart_send_success, 1);
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
		if (check_can_device_ready()) {
			break;
		}
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

	struct can_filter filter = {.mask = CAN_STD_ID_MASK};

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
		if (k_msgq_get(&mod_can_msgq, &frame, K_FOREVER) == 0) {
			mod_canrx_msg_handler(&frame);
		}
	}
}

K_THREAD_DEFINE(mod_can, 2048, mod_can_process_thread, NULL, NULL, NULL, 11, 0, 0);

void mod_heart_thread(void)
{
	struct can_frame frame = {
		.data[0] = 5,
		.id = COBID_HEATBEAT,
		.dlc = can_bytes_to_dlc(1),
	};
	int fail_count = 0, ret;

	while (true) {
		k_event_wait(&global_params.event, TIMEOUT_EVENT, false, K_FOREVER);
		k_sem_take(&heart_wake_sem, K_FOREVER);

		while (true) {
			uint32_t t1 = k_uptime_get_32();

			atomic_set(&heart_send_success, 0);

			ret = can_send(can_dev, &frame, K_MSEC(100), heart_tx_callback, NULL);

			k_sleep(K_MSEC(50));

			if (atomic_get(&heart_send_success) || ret == 0) {
				fail_count = 0;
			} else {
				fail_count++;
				LOG_WRN("heartbeat send failed, count: %d", fail_count);
			}

			if (fail_count >= 3) {
				LOG_WRN("heartbeat failed 3 times, sleeping...");
				k_sem_reset(&heart_wake_sem);
				k_event_clear(&global_params.event, TIMEOUT_EVENT);
				break;
			}
			uint32_t diff = k_uptime_get_32() - t1;

			k_sleep(K_MSEC(global_params.can_heart_time - diff));
		}
	}
}

K_THREAD_DEFINE(can_heart, 1024, mod_heart_thread, NULL, NULL, NULL, 11, 0, 0);
