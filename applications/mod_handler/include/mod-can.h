#ifndef __MOD_CAN_H__
#define __MOD_CAN_H__

#include <zephyr/kernel.h>
#include <zephyr/drivers/can.h>
#include <common.h>

enum {
	PLATFORM_RX = 0x101,
	PLATFORM_TX = 0x102,
	FW_DATA_RX = 0x103,
	RF24_CONFIG_CMD = 0x104,    /* 平台→手柄: 设置/查询 rf24 通道 */
	RF24_CONFIG_RESP = 0x105,   /* 手柄→平台: rf24 配置响应 */
	COBID_HEATBEAT = 0x763,
	TEST_FRAME = 0x777,
	HANDLER_STATE = 0x1E3,
	OVERBREAK_LASER = 0x263,
	COORD_XY = 0x363,
	COORD_Z = 0x463,
};

/* RF24_CONFIG_CMD 命令类型 */
enum rf24_config_cmd {
	RF24_CMD_SET_CHANNEL = 0x01,  /* 设置通道: [cmd 1B][channel 1B][reserved 6B] */
	RF24_CMD_GET_CONFIG  = 0x02,  /* 查询配置: [cmd 1B][reserved 7B] */
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
 * @brief 解析扫描仪 CAN 数据 (0x263/0x363/0x463)
 *
 * @param frame CAN 接收帧
 */
void mod_can_parse_scanner(struct can_frame *frame);

/**
 * @brief 处理 nRF24 配置命令 (CAN 0x104)
 *
 * @param frame CAN 接收帧 [cmd 1B][param...]
 *   RF24_CMD_SET_CHANNEL: [0x01][channel 1B][reserved 6B]
 *   RF24_CMD_GET_CONFIG:  [0x02][reserved 7B]
 */
void mod_can_rf24_config(struct can_frame *frame);
#endif
