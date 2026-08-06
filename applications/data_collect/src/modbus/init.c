#include "init.h"
#include <zephyr/app_version.h>
#include <time.h>
#ifdef CONFIG_SETTINGS
#include <zephyr/settings/settings.h>
#endif

static const uint16_t holding_regs[CONFIG_MODBUS_HOLDING_REGISTER_NUMBERS] = {
	[HOLDING_DI_EN_IDX] = 0xffff,  [HOLDING_AI_EN_IDX] = 0xf,
	[HOLDING_DI_SI_IDX] = 200,     [HOLDING_AI_SI_IDX] = 200,
	[HOLDING_HIS_SAVE_IDX] = 0,    [HOLDING_CAN_ID_IDX] = 0x111,
	[HOLDING_CAN_BPS_IDX] = 10,    [HOLDING_RS485_BPS_IDX] = 9600,
	[HOLDING_SLAVE_ID_IDX] = 0x1,  [HOLDING_IP_ADDR_1_IDX] = 192,
	[HOLDING_IP_ADDR_2_IDX] = 168, [HOLDING_IP_ADDR_3_IDX] = 12,
	[HOLDING_IP_ADDR_4_IDX] = 101, [HOLDING_HEART_TIMEOUT_IDX] = 2000,
	[HOLDING_TCP_PORT_IDX] = 502,  [HOLDING_NET_MODE_IDX] = 0,

};

int modbus_init(void)
{
	uint32_t t = time(NULL);

	update_input_reg(INPUT_VER_IDX, APP_VERSION_MAJOR << 8 | APP_VERSION_MINOR);
	for (size_t i = 0; i < ARRAY_SIZE(holding_regs); i++) {
		if (holding_regs[i]) {
			update_holding_reg(i, holding_regs[i]);
		}
	}

	update_holding_reg(HOLDING_TIMESTAMPH_IDX, t >> 16);
	update_holding_reg(HOLDING_TIMESTAMPL_IDX, t & UINT16_MAX);

#ifdef CONFIG_SETTINGS
	settings_load();
#endif
	history_enable_write(!!get_holding_reg(HOLDING_HIS_SAVE_IDX));

	/* 网络初始化 (静态 IP / DHCP) 由 net.c 的 dc_net_thread 负责,
	 * 在 SYS_INIT 之后读取 NET_MODE/IP 寄存器并配置接口. */

	return 0;
}

static K_SEM_DEFINE(sync_sem, 0, 1);
static void heart_poll(void *p)
{
	int timeout;

	while (1) {
		timeout = MAX(get_holding_reg(HOLDING_HEART_TIMEOUT_IDX), 500);
		if (k_sem_take(&sync_sem, K_MSEC(timeout))) {
			if (get_holding_reg(HOLDING_HEART_EN_IDX)) {
				update_holding_reg(HOLDING_HEART_IDX, 0);
				update_holding_reg(HOLDING_DO_IDX, 0);
				mb_set_do(0);
			}
		}
	}
}
void heart_event_send(void)
{
	k_sem_give(&sync_sem);
}

K_THREAD_DEFINE(heart, 512, heart_poll, NULL, NULL, NULL, 12, 0, 0);
SYS_INIT(modbus_init, APPLICATION, 11);
