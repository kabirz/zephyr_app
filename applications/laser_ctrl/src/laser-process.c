#include <laser-can.h>
#include <laser-flash.h>
#include <laser-rs485.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/app_version.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(laser_process, LOG_LEVEL_DBG);

static int cob_msg_send(uint8_t command, uint8_t index0, uint8_t index1,
			uint8_t subindex, uint32_t data, uint32_t id)
{
	struct can_frame frame;


	frame.data[0] = command;
	frame.data[1] = index0;
	frame.data[2] = index1;
	frame.data[3] = subindex;
	frame.data_32[1] = sys_cpu_to_be32(data);

	frame.id = id;
	frame.dlc = can_bytes_to_dlc(8);

	return laser_can_send(&frame);
}

static int laser_can_enable(bool enable)
{
	LOG_DBG("enable: %d", enable);
	if (enable) {
		laser_on();
	} else {
		laser_stopclear();
	}
	cob_msg_send(0x60, 0x41, 0x62, 0x0, enable, COB_ID1_TX);
	return 0;
}

static int laser_get_collectperiod(void)
{
	uint32_t val = 0;

	// TODO
	LOG_DBG("val: 0x%x", val);

	cob_msg_send(0x60, 0x42, 0x62, 0x0, val, COB_ID1_TX);
	return 0;
}

static int laser_set_collectperiod(uint32_t val)
{
	laser_con_measure(val);
	LOG_DBG("val: 0x%x", val);
	cob_msg_send(0x60, 0x42, 0x62, 0x0, 1, COB_ID1_TX);
	return 0;
}

static int laser_mem_writemode(void)
{
	laser_flash_write_mode();
	LOG_DBG("mem write mode");
	cob_msg_send(0x00, 0x0, 0x0, 0x1, 0x22000000, COB_ID1_TX);
	return 0;
}

static int laser_mem_readmode(void)
{
	laser_flash_read_mode();
	LOG_DBG("mem read mode");
	cob_msg_send(0x00, 0x0, 0x0, 0x1, 0x40000000, COB_ID1_TX);
	return 0;
}

static int laser_mem_writedata(uint16_t address, uint32_t val)
{
	int ret = laser_flash_write(address, val);

	LOG_DBG("address: %d, val: 0x%x", address, val);

	if (ret == 0) {
		cob_msg_send(0x00, 0x0, 0x0, 0x1, val, COB_ID2_TX);
	} else {
		cob_msg_send(0x00, 0x0, 0x0, 0x12, ret, COB_ID2_TX);
	}
	return 0;
}

static int laser_mem_readdata(uint16_t address)
{
	uint32_t val;
	int ret = laser_flash_read(address, &val);

	LOG_DBG("address: %d, val: 0x%x", address, val);
	if (ret == 0) {
		cob_msg_send(0x00, 0x0, 0x0, 0x1, val, COB_ID2_TX);
	} else {
		cob_msg_send(0x00, 0x0, 0x0, 0x12, ret, COB_ID2_TX);
	}
	return 0;
}

int cob_id664_process(struct can_frame *frame)
{

	if (frame->data[0] == 0x22) {
		if (frame->data[1] == 0x0 && frame->data[2] == 0x0) {
			laser_mem_writemode();
		} else if (frame->data[1] == 0x41 && frame->data[2] == 0x62) {
			switch (sys_be32_to_cpu(frame->data_32[1])) {
			case 0:
				laser_can_enable(false);
				break;
			case 1:
				laser_can_enable(true);
				break;
			}
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

	return 0;
}

int cob_id665_process(struct can_frame *frame)
{

	if (frame->data[0] == 0x22) {
		uint32_t address = frame->data[1] + frame->data[2] * 10;
		laser_mem_writedata(address, sys_be32_to_cpu(frame->data_32[1]));
	} else if (frame->data[0] == 0x40) {
		uint32_t address = frame->data[1] + frame->data[2] * 10;
		laser_mem_readdata(address);
	}

	return 0;
}

static void fw_can_recevie(uint32_t offset, uint32_t code)
{
	struct can_frame frame;

	frame.id = FW_UP_TX;
	frame.data_32[0] = sys_cpu_to_be32(code);
	frame.data_32[1] = sys_cpu_to_be32(offset);
	frame.dlc = can_bytes_to_dlc(8);
	frame.flags = 0;

	laser_can_send(&frame);

}

int fw_update(struct can_frame *frame)
{
	static struct image_fw_msg msg;
	uint32_t size;

	if (frame->id == FW_UP_START) {
		memset(&msg, 0, sizeof(msg));
		if (flash_img_init(&msg.flash_img_ctx) != 0) {
			LOG_ERR("flash init failed");
			fw_can_recevie(0, FW_CODE_FLASH_ERROR);
			return -1;
		}
		if (flash_area_flatten(msg.flash_img_ctx.flash_area, 0, msg.flash_img_ctx.flash_area->fa_size) != 0) {
			LOG_ERR("flash erase failed");
			fw_can_recevie(0, FW_CODE_FLASH_ERROR);
			return -1;
		}
		msg.offset = 0;
		msg.total_size = sys_be32_to_cpu(frame->data_32[0]);
		fw_can_recevie(0, FW_CODE_OFFSET);
	} else if (frame->id == FW_UP_DATA) {
		size = can_dlc_to_bytes(frame->dlc);
		if (flash_img_buffered_write(&msg.flash_img_ctx, frame->data, size, false)) {
			fw_can_recevie(0, FW_CODE_FLASH_ERROR);
			return -1;
		}
		msg.offset += size;
		if (msg.offset == msg.total_size) {
			LOG_INF("recived firmware: %d/%d", msg.offset, msg.total_size);
			fw_can_recevie(msg.total_size, FW_CODE_UPDATE_SUCCESS);
		} else {
			if (msg.offset % 512 == 0)
				LOG_INF("recived firmware: %d/%d", msg.offset, msg.total_size);
			fw_can_recevie(msg.offset, FW_CODE_OFFSET);
		}
	} else if (frame->id == FW_UP_CONFIRM) {
		flash_img_buffered_write(&msg.flash_img_ctx, frame->data, 0, true);
		if (msg.total_size != flash_img_bytes_written(&msg.flash_img_ctx)) {
			LOG_ERR("Download failed total: %d, %d", msg.total_size, flash_img_bytes_written(&msg.flash_img_ctx));
			fw_can_recevie(msg.offset, FW_CODE_TRANFER_ERROR);
			return -1;
		} else {
			LOG_INF("Download Finished, Now system will reboot and upgrade...");
		}
		boot_write_img_confirmed();
		fw_can_recevie(0x55AA55AA, FW_CODE_CONFIRM);
		k_sleep(K_SECONDS(5));
		/* sys_reboot(SYS_REBOOT_COLD); */
	} else if (frame->id == FW_GET_VER) {
		fw_can_recevie(APPVERSION, FW_CODE_VERSION);
	}

	return 0;
}
