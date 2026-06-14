#include <fw_upgrade.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/app_version.h>
#include <zephyr/sys/reboot.h>

LOG_MODULE_REGISTER(fw_upgrade, LOG_LEVEL_INF);

#define FLASH_BUFFER_SIZE 64

static const struct device *flash_dev = DEVICE_DT_GET(DT_NODELABEL(flash));

static uint8_t flash_buffer[FLASH_BUFFER_SIZE];
static uint16_t flash_buffer_index = 0;
static uint32_t current_flash_addr = FLASH_APP_START_ADDR;
static uint32_t total_fw_size = 0;
static uint32_t received_fw_size = 0;

static fw_send_response_t response_cb;

void fw_set_response_cb(fw_send_response_t cb)
{
	response_cb = cb;
}

static void fw_send_response(uint32_t code, uint32_t value)
{
	if (response_cb) {
		response_cb(code, value);
	}
}

int fw_update(struct can_frame *frame)
{
	uint32_t size = can_dlc_to_bytes(frame->dlc);

	if (frame->id == CAN_ID_PLATFORM_RX) {
		if (frame->data_32[0] == BOARD_START_UPDATE) {
			if (size != 8) {
				LOG_ERR("start update message size must 8");
				fw_send_response(FW_CODE_FLASH_ERROR, 0);
				return -1;
			}
			total_fw_size = frame->data_32[1];
			received_fw_size = 0;
			current_flash_addr = FLASH_APP_START_ADDR;
			flash_buffer_index = 0;

			uint32_t erase_size = total_fw_size;
			if (erase_size > (FLASH_APP_END_ADDR - FLASH_APP_START_ADDR)) {
				erase_size = FLASH_APP_END_ADDR - FLASH_APP_START_ADDR;
			}

			uint32_t erase_addr = FLASH_APP_START_ADDR;
			LOG_INF("Start upgrade, size: %d bytes", total_fw_size);

			while (erase_size > 0) {
				struct flash_pages_info page_info;
				if (flash_get_page_info_by_offs(flash_dev, erase_addr, &page_info) != 0) {
					LOG_ERR("Failed to get page info");
					fw_send_response(FW_CODE_FLASH_ERROR, 0);
					return -1;
				}
				if (flash_erase(flash_dev, page_info.start_offset, page_info.size) != 0) {
					LOG_ERR("Erase failed");
					fw_send_response(FW_CODE_FLASH_ERROR, 0);
					return -1;
				}
				if (page_info.size <= erase_size) {
					erase_size -= page_info.size;
				} else {
					erase_size = 0;
				}
				erase_addr += page_info.size;
			}

			fw_send_response(FW_CODE_OFFSET, 0);
		} else if (frame->data_32[0] == BOARD_CONFIRM) {
			if (flash_buffer_index > 0) {
				flash_write(flash_dev, current_flash_addr, flash_buffer, flash_buffer_index);
				current_flash_addr += flash_buffer_index;
				flash_buffer_index = 0;
			}
			if (received_fw_size != total_fw_size) {
				LOG_ERR("Download failed: expected %d, got %d", total_fw_size, received_fw_size);
				fw_send_response(FW_CODE_TRANFER_ERROR, received_fw_size);
				return -1;
			}
			LOG_INF("Download finished, jumping to app...");
			fw_send_response(FW_CODE_CONFIRM, 0x55AA55AA);
			k_msleep(100);
			JumpToApp(APP_START_ADDR);
		} else if (frame->data_32[0] == BOARD_VERSION) {
			LOG_INF("Version request: %s", APP_VERSION_STRING);
			fw_send_response(FW_CODE_VERSION, APPVERSION);
		} else if (frame->data_32[0] == BOARD_REBOOT) {
			LOG_INF("Reboot requested");
			sys_reboot(SYS_REBOOT_WARM);
		}
	} else if (frame->id == CAN_ID_FW_DATA_RX) {
		if (received_fw_size >= total_fw_size) {
			LOG_WRN("Extra data received");
			return -1;
		}

		for (uint32_t i = 0; i < size && received_fw_size < total_fw_size; i++) {
			flash_buffer[flash_buffer_index++] = frame->data[i];
			received_fw_size++;

			if (flash_buffer_index >= FLASH_BUFFER_SIZE || received_fw_size >= total_fw_size) {
				if (flash_write(flash_dev, current_flash_addr, flash_buffer, flash_buffer_index) != 0) {
					LOG_ERR("Flash write failed at 0x%08x", current_flash_addr);
					fw_send_response(FW_CODE_FLASH_ERROR, received_fw_size);
					return -1;
				}
				current_flash_addr += flash_buffer_index;
				flash_buffer_index = 0;

				if (received_fw_size >= total_fw_size) {
					LOG_INF("Receive complete: %d/%d", received_fw_size, total_fw_size);
					fw_send_response(FW_CODE_UPDATE_SUCCESS, received_fw_size);
				} else if (received_fw_size % 4096 == 0) {
					LOG_INF("Progress: %d/%d", received_fw_size, total_fw_size);
				} else if (received_fw_size % 64 == 0) {
					fw_send_response(FW_CODE_OFFSET, received_fw_size);
				}
			}
		}
	}
	return 0;
}

uint8_t VerifyAppFirmware(void)
{
	uint32_t app_stack_ptr = 0;
	uint32_t app_reset_vec = 0;

	flash_read(flash_dev, FLASH_APP_START_ADDR, &app_stack_ptr, sizeof(app_stack_ptr));
	flash_read(flash_dev, FLASH_APP_START_ADDR + 4, &app_reset_vec, sizeof(app_reset_vec));

	LOG_INF("Verify: stack=0x%08x, reset=0x%08x", app_stack_ptr, app_reset_vec);

	if ((app_stack_ptr >= 0x20000000) && (app_stack_ptr < 0x2000C000) &&
	    (app_reset_vec >= FLASH_APP_START_ADDR) && (app_reset_vec < FLASH_APP_END_ADDR)) {
		return 1;
	}
	return 0;
}

uint8_t CheckUpgradeFlag(void)
{
	uint32_t flag = 0;
	flash_read(flash_dev, FLASH_FLAG_ADDR, &flag, sizeof(flag));
	return (flag == UPGRADE_FLAG_VALUE) ? 1 : 0;
}

void ClearUpgradeFlag(void)
{
	struct flash_pages_info page_info;

	flash_get_page_info_by_offs(flash_dev, FLASH_FLAG_ADDR, &page_info);
	flash_erase(flash_dev, page_info.start_offset, page_info.size);
}

void JumpToApp(uint32_t app_addr)
{
	typedef void (*pFunction)(void);
	pFunction jump_to_app;

	irq_lock();

	SCB->VTOR = app_addr;

	uint32_t jump_addr = *(volatile uint32_t *)(app_addr + 4);
	jump_to_app = (pFunction)jump_addr;

	__set_MSP(*(volatile uint32_t *)app_addr);

	__DSB();
	__ISB();

	jump_to_app();
}
