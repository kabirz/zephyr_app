#ifndef __MOD_CAN_H__
#define __MOD_CAN_H__

#include <zephyr/kernel.h>
#include <zephyr/drivers/can.h>
#include <common.h>

enum {
	PLATFORM_RX = 0x101,
	PLATFORM_TX = 0x102,
	FW_DATA_RX = 0x103,
	COBID_HEATBEAT = 0x763,
	HANDLER_STATE = 0x1E3,
	OVERBREAK_LASER = 0x263,
	COORD_XY = 0x363,
	COORD_Z = 0x463,
	LORA_CONFIG_RX = 0x105,
	LORA_CONFIG_TX = 0x106,
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
	LORA_CMD_SET_MODE   = 0x01, /* 设置 mode+prot */
	LORA_CMD_QUERY_MODE = 0x02, /* 查询 mode+prot */
	LORA_CMD_SET_CH1    = 0x03, /* 设置通道1 spd1+ch1 */
	LORA_CMD_QUERY_CH1  = 0x04, /* 查询通道1 */
	LORA_CMD_SET_CH2    = 0x05, /* 设置通道2 spd2+ch2 */
	LORA_CMD_QUERY_CH2  = 0x06, /* 查询通道2 */
	LORA_CMD_QUERY_NID  = 0x07, /* 查询节点 ID */
	LORA_CMD_SET_NID    = 0x08, /* 设置节点 ID */
	LORA_CMD_QUERY_GWID = 0x09, /* 查询网关 ID */
	LORA_CMD_SET_GWID   = 0x0A, /* 设置网关 ID */
	LORA_CMD_QUERY_PNUM = 0x0B, /* 查询通道选择 */
	LORA_CMD_SET_PNUM   = 0x0C, /* 设置通道选择 */
	LORA_CMD_SET_TEST   = 0x0D, /* 进入/退出测试模式 */
	LORA_CMD_SET_POWER  = 0x0F, /* 开启/关闭Lora */
};

/* LoRa 配置结果 */
enum lora_config_result {
	LORA_CFG_OK = 0x00,
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
 * @brief 通过 CAN 发送手柄状态帧 (0x1E3, 大端序)
 *
 * @param params 全局参数
 * @return 0 成功, 负数失败
 */
int mod_can_send_handler_state(const global_params_t *params);

/**
 * @brief 解析扫描仪 CAN 数据 (0x263/0x363/0x463)
 *
 * @param frame CAN 接收帧
 */
void mod_can_parse_scanner(struct can_frame *frame);
#endif
