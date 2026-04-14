#ifndef __MOD_CAN_H__
#define __MOD_CAN_H__

#include <zephyr/kernel.h>
#include <zephyr/drivers/can.h>
#include <common.h>

enum {
	PLATFORM_RX     = 0x101,
	PLATFORM_TX     = 0x102,
	FW_DATA_RX      = 0x103,
	COBID_HEATBEAT  = 0x763,
	COBID_TELEMETRY = 0x764,
	LORA_CONFIG_RX  = 0x105,
	LORA_CONFIG_TX  = 0x106,
};

enum fw_error_code {
	FW_CODE_OFFSET,
	FW_CODE_UPDATE_SUCCESS,
	FW_CODE_VERSION,
	FW_CODE_CONFIRM,
	FW_CODE_FLASH_ERROR,
	FW_CODE_TRANFER_ERROR,
};

enum board_option {
	BOARD_START_UPDATE,
	BOARD_CONFIRM,
	BOARD_VERSION,
	BOARD_REBOOT,
};

enum {
	LASER_DEVICE_STATUS,
	LASER_WRITE_MODE,
	LASER_NEED_CLOSE,
	LASER_ON,
	LASER_CON_MESURE,
	LASER_FW_UPDATE,
};

/* LoRa 配置命令 */
enum lora_config_cmd {
	LORA_CMD_SET   = 0x01,
	LORA_CMD_QUERY = 0x02,
};

/* LoRa 配置结果 */
enum lora_config_result {
	LORA_CFG_OK   = 0x00,
	LORA_CFG_FAIL = 0x01,
};

#define CAN_HEART_TIME 400

#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/storage/flash_map.h>

struct image_fw_msg {
	uint32_t total_size;
	uint32_t offset;
	struct flash_img_context flash_img_ctx;
};
int fw_update(struct can_frame *frame);
int mod_can_send(struct can_frame *frame);

/**
 * @brief 通过 CAN 发送遥测帧 (X/Y 角度 + 按键 + 电量)
 *
 * @param params 全局参数
 * @return 0 成功, 负数失败
 */
int mod_can_send_telemetry(const gloval_params_t *params);
#endif
