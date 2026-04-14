#include <zephyr/kernel.h>
#include <mod-can.h>
#include <lora.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
#include <common.h>
LOG_MODULE_REGISTER(mod_can, LOG_LEVEL_INF);

static const struct device *can_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_canbus));
CAN_MSGQ_DEFINE(mod_can_msgq, 8);

static K_SEM_DEFINE(heart_wake_sem, 1, 1);
static atomic_t heart_send_success = ATOMIC_INIT(0);

static void can_lora_config_handler(struct can_frame *frame);

static void mod_canrx_msg_handler(struct can_frame *frame)
{
	k_sem_give(&heart_wake_sem);

	switch (frame->id) {
	case PLATFORM_RX:
	case FW_DATA_RX:
		fw_update(frame);
		break;
	case LORA_CONFIG_RX:
		can_lora_config_handler(frame);
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

/* ================================================================
 * LoRa 参数配置 — CAN 远程设置/查询, k_work 异步执行
 *   lora_gw_configure() 涉及 AT 模式握手 + 模块重启 (10s+),
 *   不能在 CAN 接收线程中同步执行, 提交到系统工作队列
 * ================================================================ */
static struct k_work lora_cfg_work;
static struct lora_gw_config pending_lora_cfg;
static uint8_t lora_cfg_cmd;

static void lora_cfg_work_handler(struct k_work *work)
{
	struct can_frame resp = {
		.id = LORA_CONFIG_TX,
		.dlc = can_bytes_to_dlc(6),
	};

	if (lora_cfg_cmd == LORA_CMD_SET) {
		int ret = lora_gw_configure(&pending_lora_cfg);

		resp.data[0] = (ret == 0) ? LORA_CFG_OK : LORA_CFG_FAIL;
		LOG_INF("LoRa config SET: ret=%d mode=%d spd=%d ch=%d nid=%d",
			ret, pending_lora_cfg.mode, pending_lora_cfg.spd,
			pending_lora_cfg.ch, pending_lora_cfg.nid);
		mod_can_send(&resp);
	} else if (lora_cfg_cmd == LORA_CMD_QUERY) {
		struct lora_gw_config cfg;
		int ret = lora_gw_query(&cfg);

		resp.data[0] = (ret == 0) ? LORA_CFG_OK : LORA_CFG_FAIL;
		if (ret == 0) {
			resp.data[1] = (uint8_t)cfg.mode;
			resp.data[2] = cfg.spd;
			resp.data[3] = cfg.ch;
			memcpy(&resp.data[4], &cfg.nid, sizeof(cfg.nid));
			LOG_INF("LoRa config QUERY: mode=%d spd=%d ch=%d nid=%d",
				cfg.mode, cfg.spd, cfg.ch, cfg.nid);
		} else {
			LOG_ERR("LoRa config QUERY failed: %d", ret);
		}
		mod_can_send(&resp);
	}
}

static void can_lora_config_handler(struct can_frame *frame)
{
	lora_cfg_cmd = frame->data[0];

	if (lora_cfg_cmd == LORA_CMD_SET) {
		pending_lora_cfg.mode = (frame->data[1] == 1)
			? LORA_GW_MODE_NETWORK : LORA_GW_MODE_TRANS;
		pending_lora_cfg.spd = frame->data[2];
		pending_lora_cfg.ch = frame->data[3];
		memcpy(&pending_lora_cfg.nid, &frame->data[4], sizeof(uint16_t));
		k_work_submit(&lora_cfg_work);
	} else if (lora_cfg_cmd == LORA_CMD_QUERY) {
		k_work_submit(&lora_cfg_work);
	} else {
		LOG_ERR("Unknown LoRa config cmd: 0x%02x", lora_cfg_cmd);
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

	filter.id = LORA_CONFIG_RX;
	can_add_rx_filter_msgq(can_dev, &mod_can_msgq, &filter);

	k_work_init(&lora_cfg_work, lora_cfg_work_handler);
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
				global_params.connect_type = CAN_TYPE;
				mod_can_send_telemetry(&global_params);
			} else {
				fail_count++;
				LOG_WRN("heartbeat send failed, count: %d", fail_count);
			}

			if (fail_count >= 3) {
				LOG_WRN("heartbeat failed 3 times, switching to LoRa");
				global_params.connect_type = LORA_TYPE;
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

/* ================================================================
 * CAN 遥测帧发送 — X/Y 角度 + 按键 + 电量
 *
 * 帧 ID: 0x764 (COBID_TELEMETRY), DLC: 6
 * Data[0-1]: X 角度 (int16_t LE, 0.1° 单位)
 * Data[2-3]: Y 角度 (int16_t LE, 0.1° 单位)
 * Data[4]:   按键 (0/1)
 * Data[5]:   电量 (0~100)
 * ================================================================ */
int mod_can_send_telemetry(const gloval_params_t *params)
{
	int16_t x_raw = (int16_t)(params->x_degree * 10);
	int16_t y_raw = (int16_t)(params->y_degree * 10);

	struct can_frame frame = {
		.id = COBID_TELEMETRY,
		.dlc = can_bytes_to_dlc(6),
	};

	memcpy(&frame.data[0], &x_raw, sizeof(x_raw));
	memcpy(&frame.data[2], &y_raw, sizeof(y_raw));
	frame.data[4] = params->h_button ? 1 : 0;
	frame.data[5] = params->power_level;

	return mod_can_send(&frame);
}
