#include <laser-can.h>
#include <laser-flash.h>
#include <laser-rs485.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/app_version.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(laser_process, LOG_LEVEL_INF);

static int cob_msg_send(uint8_t command, uint16_t index,
			uint8_t subindex, uint32_t data, uint32_t id)
{
	struct can_frame frame;
	struct can_cob_msg *cob_msg = (void *)frame.data;


	cob_msg->command = 0x60;
	cob_msg->index = sys_cpu_to_be16(0x4162);
	cob_msg->subindex = 0;
	cob_msg->data = sys_cpu_to_be32(data);
	frame.id = id;
	frame.dlc = can_bytes_to_dlc(8);
	return laser_can_send(&frame);
}

static int laser_can_enable(bool enable)
{
	if (enable) {
		laser_on();
	} else {
		laser_stopclear();
	}
	cob_msg_send(0x60, 0x4162, 0x0, enable, COB_ID1_TX);
	return 0;
}

static int laser_get_collectperiod(void)
{
	uint32_t val = 0;

	// TODO

	cob_msg_send(0x60, 0x4262, 0x0, val, COB_ID1_TX);
	return 0;
}

static int laser_set_collectperiod(uint32_t val)
{
	laser_con_measure(val);
	cob_msg_send(0x60, 0x4262, 0x0, 1, COB_ID1_TX);
	return 0;
}

static int laser_mem_writemode(void)
{
	laser_flash_write_mode();
	cob_msg_send(0x00, 0x0, 0x1, 0x22000000, COB_ID1_TX);
	return 0;
}

static int laser_mem_readmode(void)
{
	laser_flash_read_mode();
	cob_msg_send(0x00, 0x0, 0x1, 0x40000000, COB_ID1_TX);
	return 0;
}

static int laser_mem_writedata(uint16_t address, uint32_t val)
{
	int ret = laser_flash_write(address, val);

	if (ret == 0) {
		cob_msg_send(0x00, 0x0, 0x1, val, COB_ID2_TX);
	} else {
		cob_msg_send(0x00, 0x0, 0x12, ret, COB_ID2_TX);
	}
	return 0;
}

static int laser_mem_readdata(uint16_t address)
{
	uint32_t val;
	int ret = laser_flash_read(address, &val);

	if (ret == 0) {
		cob_msg_send(0x00, 0x0, 0x1, val, COB_ID2_TX);
	} else {
		cob_msg_send(0x00, 0x0, 0x1, ret, COB_ID2_TX);
	}
	return 0;
}

int cob_id664_process(void *data)
{
	struct can_cob_msg *msg = data;

	if (msg->command == 0x22) {
		if (sys_be16_to_cpu(msg->index) == 0x00) {
			laser_mem_writemode();
		} else if (sys_be16_to_cpu(msg->index) == 0x4162) {
			switch (sys_be32_to_cpu(msg->data)) {
			case 0:
				laser_can_enable(false);
				break;
			case 1:
				laser_can_enable(true);
				break;
			}
		} else if (sys_be16_to_cpu(msg->index) == 0x4262) {
			laser_set_collectperiod(sys_be32_to_cpu(msg->data));
		}
	} else if (msg->command == 0x40) {
		if (sys_be16_to_cpu(msg->index) == 0x00) {
			laser_mem_readmode();
		} else if (sys_be16_to_cpu(msg->index) == 0x4262) {
			laser_get_collectperiod();
		}
	}

	return 0;
}

int cob_id665_process(void *data)
{
	struct can_cob_msg *msg = data;

	if (msg->command == 0x22) {
		uint32_t address = (msg->index >> 8) * 10 + (msg->index & 0xff);
		laser_mem_writedata(address, sys_be32_to_cpu(msg->data));
	} else if (msg->command == 0x40) {
		uint32_t address = (msg->index >> 8) * 10 + (msg->index & 0xff);
		laser_mem_readdata(address);
	}

	return 0;
}

static void fw_can_recevie(uint32_t offset, uint32_t code)
{
	struct can_frame frame;

	frame.id = FW_UP_TX;
	frame.data_32[0] = code;
	frame.data_32[1] = offset;
	frame.dlc = can_bytes_to_dlc(8);

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
		msg.total_size = 0;
		fw_can_recevie(0, FW_CODE_OFFSET);
	} else if (frame->id == FW_UP_DATA) {
		size = can_dlc_to_bytes(frame->dlc);
		if (flash_img_buffered_write(&msg.flash_img_ctx, frame->data, size, false)) {
			fw_can_recevie(0, FW_CODE_FLASH_ERROR);
			return -1;
		}
		msg.offset += size;
		printk("recived firmware: %d/%d\n\r", msg.offset, msg.total_size);
		if (msg.offset == msg.total_size)
			fw_can_recevie(msg.total_size, FW_CODE_UPDATE_SUCCESS);
		else
			fw_can_recevie(msg.offset, FW_CODE_OFFSET);
	} else if (frame->id == FW_UP_CONFIRM) {
		if (msg.total_size != flash_img_bytes_written(&msg.flash_img_ctx)) {
			LOG_ERR("Download failed");
		} else {
			LOG_INF("Download Finished, Now system will reboot and upgrade...");
		}
		flash_img_buffered_write(&msg.flash_img_ctx, frame->data, 0, false);
		boot_write_img_confirmed();
		fw_can_recevie(0x55AA55AA, FW_CODE_CONFIMR);
		k_sleep(K_SECONDS(5));
		sys_reboot(SYS_REBOOT_COLD);
	} else if (frame->id == FW_GET_VER) {
		fw_can_recevie(APPVERSION, FW_CODE_VERSION);
	}

	return 0;
}
