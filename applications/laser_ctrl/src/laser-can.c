#include <zephyr/kernel.h>
#include <laser-can.h>
#include <laser-rs485.h>
#include <laser-flash.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(laser_can, LOG_LEVEL_INF);

static const struct device *can_dev = DEVICE_DT_GET(DT_NODELABEL(can1));


static uint32_t frame_count;
K_MSGQ_DEFINE(laser_can_msgq, sizeof(struct laser_can_msg), 10, 4);

static void laser_cantx_callback(const struct device *dev, int error, void *user_data);
static void laser_canrx_callback(const struct device *dev, struct can_frame *frame, void *user_data);

static void laser_canrx_msg_handler(struct can_frame *frame)
{
	switch (frame->id) {
	case COB_ID1_RX:
		cob_id664_process(frame);
		break;
	case COB_ID2_RX:
		cob_id665_process(frame);
		break;
	case FW_UP_START:
	case FW_UP_DATA:
	case FW_UP_CONFIRM:
	case FW_GET_VER:
		fw_update(frame);
		break;
	default:
		LOG_ERR("can frame id (0x%x) is not support", frame->id);
	}
}

static void can_msg_process(struct k_work *work)
{
	struct laser_can_msg can_msg;
	int ret;

	if ((ret = k_msgq_get(&laser_can_msgq, &can_msg, K_NO_WAIT)) != 0) {
		LOG_ERR("error msg: msgq error: ret: %d", ret);
		return;
	}
	switch (can_msg.type) {
		case CAN_RX:
			laser_canrx_msg_handler(&can_msg.frame);
			break;
		case CAN_SEND:
			frame_count += 1;
			can_msg.count = frame_count;
			can_send(can_dev, &can_msg.frame, K_NO_WAIT, laser_cantx_callback, NULL);
			break;
		case CAN_TX_COM:
			if (can_msg.error == 0) {
				LOG_DBG("CAN frame #%u successfully sent", can_msg.count);
			} else {
				LOG_ERR("failed to send CAN frame #%u (err %d)", can_msg.count, can_msg.error);
			}
			break;
		default:
			LOG_ERR("error can msg");
			break;
	}
}
static K_WORK_DEFINE(laser_can_work, can_msg_process);

int laser_can_send(struct can_frame *frame)
{
	struct laser_can_msg lmsg;

	lmsg.type = CAN_SEND;
	lmsg.frame = *frame;

	k_msgq_put(&laser_can_msgq, &lmsg, K_NO_WAIT);
	k_work_submit(&laser_can_work);
	return 0;
}

static void laser_cantx_callback(const struct device *dev, int error, void *user_data)
{
	struct laser_can_msg msg;

	ARG_UNUSED(dev);
	msg.error = error;
	msg.type = CAN_TX_COM;

	k_msgq_put(&laser_can_msgq, &msg, K_NO_WAIT);
	k_work_submit(&laser_can_work);
}

static void laser_canrx_callback(const struct device *dev, struct can_frame *frame, void *user_data)
{
	struct laser_can_msg msg;
	msg.type = CAN_RX;
	msg.frame = *frame;

	k_msgq_put(&laser_can_msgq, &msg, K_NO_WAIT);
	k_work_submit(&laser_can_work);
}

int laser_can_init(void)
{
	int err = 0;
	struct can_filter filter = {0};

	if (!device_is_ready(can_dev)) {
		LOG_ERR("can device (%s) is not ready", can_dev->name);
		LOG_ERR("%d, %d",  can_dev->state->initialized, can_dev->state->init_res);
		return -1;
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
	filter.id = COB_ID1_RX;
	filter.mask = CAN_STD_ID_MASK;
	err = can_add_rx_filter(can_dev, laser_canrx_callback, NULL, &filter);
	filter.id = COB_ID2_RX;
	err = can_add_rx_filter(can_dev, laser_canrx_callback, NULL, &filter);
	filter.id = FW_UP_START;
	err = can_add_rx_filter(can_dev, laser_canrx_callback, NULL, &filter);
	filter.id = FW_UP_DATA;
	err = can_add_rx_filter(can_dev, laser_canrx_callback, NULL, &filter);
	filter.id = FW_UP_CONFIRM;
	err = can_add_rx_filter(can_dev, laser_canrx_callback, NULL, &filter);
	filter.id = FW_GET_VER;
	err = can_add_rx_filter(can_dev, laser_canrx_callback, NULL, &filter);
end:
	return err;
}

SYS_INIT(laser_can_init, APPLICATION, 32);


void laser_data_process_handler(void)
{
	while (true) {
		k_sleep(K_MSEC(500));
		if (read_mode && laser_enabled) {
			// send data
		}
	}
}

K_THREAD_DEFINE(laser_data, 1024, laser_data_process_handler, NULL, NULL, NULL, 12, 0, 0);
