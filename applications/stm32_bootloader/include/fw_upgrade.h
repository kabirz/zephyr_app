#ifndef __FW_UPGRADE_H
#define __FW_UPGRADE_H

#include <stdint.h>
#include <zephyr/drivers/can.h>

#define FLASH_APP_START_ADDR      0x08010000
#define FLASH_APP_END_ADDR        0x08040000
#define FLASH_SECTOR_SIZE         0x400
#define FLASH_FLAG_ADDR           0x0803F800
#define UPGRADE_FLAG_VALUE        0x55AADEAD
#define APP_START_ADDR            0x08010000

#define CAN_ID_PLATFORM_RX        0x101
#define CAN_ID_PLATFORM_TX        0x102
#define CAN_ID_FW_DATA_RX         0x103

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

typedef void (*fw_send_response_t)(uint32_t code, uint32_t value);

void fw_set_response_cb(fw_send_response_t cb);
int fw_update(struct can_frame *frame);
uint8_t VerifyAppFirmware(void);
uint8_t CheckUpgradeFlag(void);
void ClearUpgradeFlag(void);
void JumpToApp(uint32_t app_addr);

#endif
