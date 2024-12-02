
#include "init.h"
#include <zephyr/sys/util.h>
#include <zephyr/modbus/modbus.h>
#include <zephyr/kernel.h>

static struct modbus_iface_param server_param = {
	.mode = MODBUS_MODE_RTU,
	.server =
		{
			.user_cb = &mbs_cbs,
			.unit_id = 1,
		},
	.serial =
		{
			.baud = 9600,
			.parity = UART_CFG_PARITY_NONE,
		},
};

#define MODBUS_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(zephyr_modbus_serial)

static int init_modbus_server(void)
{
	const char iface_name[] = {DEVICE_DT_NAME(MODBUS_NODE)};
	int iface;

	iface = modbus_iface_get_by_name(iface_name);

	if (iface < 0) {
		LOG_ERR("Failed to get iface index for %s", iface_name);
		return iface;
	}

	server_param.server.unit_id = get_holding_reg(HOLDING_SLAVE_ID_IDX);
	server_param.serial.baud = get_holding_reg(HOLDING_RS485_BPS_IDX);

	return modbus_init_server(iface, server_param);
}

int rtu_init(void)
{
	if (init_modbus_server()) {
		LOG_ERR("Modbus RTU server initialization failed");
	}
	return 0;
}

SYS_INIT(rtu_init, APPLICATION, 13);
