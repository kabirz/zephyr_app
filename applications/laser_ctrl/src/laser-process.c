#include <laser-can.h>
#include <laser-flash.h>
#include <laser-rs485.h>
#include <zephyr/sys/byteorder.h>
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

