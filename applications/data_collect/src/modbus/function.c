#include "init.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/modbus/modbus.h>
#include <zephyr/sys/reboot.h>
#ifdef CONFIG_SETTINGS
#include <zephyr/settings/settings.h>
#endif

static uint16_t holding_reg[CONFIG_MODBUS_HOLDING_REGISTER_NUMBERS];
static uint16_t input_reg[CONFIG_MODBUS_INPUT_REGISTER_NUMBERS];
static uint8_t coils_state[CONFIG_MODBUS_COLS_REGISTER_NUMBERS];

static int coil_rd(uint16_t addr, bool *state)
{
	if (addr >= ARRAY_SIZE(coils_state)) {
		return -ENOTSUP;
	}

	if (coils_state[addr / 8] & BIT(addr % 8)) {
		*state = true;
	} else {
		*state = false;
	}

	LOG_DBG("Coil read, addr %u, %d", addr, (int)*state);

	return 0;
}

static int coil_wr(uint16_t addr, bool state)
{
	bool on;

	if (addr >= ARRAY_SIZE(coils_state)) {
		return -ENOTSUP;
	}

	if (state == true) {
		coils_state[addr / 8] |= BIT(addr % 8);
		on = true;
	} else {
		coils_state[addr / 8] &= ~BIT(addr % 8);
		on = false;
	}

	LOG_DBG("Coil write, addr %u, %d", addr, (int)state);

	return 0;
}

static int holding_reg_rd(uint16_t addr, uint16_t *reg)
{
	if (addr >= ARRAY_SIZE(holding_reg)) {
		return -ENOTSUP;
	}

	*reg = holding_reg[addr];

	LOG_DBG("Holding register read, addr %u", addr);

	return 0;
}

uint16_t get_holding_reg(uint16_t addr)
{
	if (addr >= ARRAY_SIZE(holding_reg)) {
		return 0;
	}
	return holding_reg[addr];
}

int update_holding_reg(uint16_t addr, uint16_t reg)
{
	if (addr >= ARRAY_SIZE(holding_reg)) {
		return -ENOTSUP;
	}

	holding_reg[addr] = reg;

	return 0;
}

static int holding_reg_wr(uint16_t addr, uint16_t reg)
{
	if (addr >= ARRAY_SIZE(holding_reg)) {
		return -ENOTSUP;
	}

	holding_reg[addr] = reg;
	switch (addr) {
	case HOLDING_DO_IDX:
		reg = reg & 0xff;
		mb_set_do(reg);
		break;
	case HOLDING_HIS_SAVE_IDX:
		history_enable_write(!!reg);
		break;
	case HOLDING_TIMESTAMPH_IDX:
	case HOLDING_TIMESTAMPL_IDX:
		uint32_t val = (uint32_t)(holding_reg[HOLDING_TIMESTAMPH_IDX]) << 16 |
			       holding_reg[HOLDING_TIMESTAMPL_IDX];
		set_timestamp((time_t)val);
		break;
	case HOLDING_CFG_SAVE_IDX:
		holding_reg[addr] = 0;
#ifdef CONFIG_SETTINGS
		settings_save();
#endif
		break;
	case HOLDING_REBOOT_IDX:
		sys_reboot(SYS_REBOOT_COLD);
		break;
	default:
		break;
	}

	LOG_DBG("Holding register write, addr %u", addr);

	return 0;
}

int update_input_reg(uint16_t addr, uint16_t reg)
{
	if (addr >= ARRAY_SIZE(input_reg)) {
		return -ENOTSUP;
	}
	input_reg[addr] = reg;

	LOG_DBG("set input register addr %u, value: %u", addr, reg);

	return 0;
}

static int input_reg_rd(uint16_t addr, uint16_t *reg)
{
	if (addr >= ARRAY_SIZE(input_reg)) {
		return -ENOTSUP;
	}

	*reg = input_reg[addr];

	LOG_DBG("Input register read, addr %u", addr);

	return 0;
}

uint16_t get_input_reg(size_t index)
{
	if (index < CONFIG_MODBUS_INPUT_REGISTER_NUMBERS) {
		return input_reg[index];
	}
	return 0;
}

struct modbus_user_callbacks mbs_cbs = {
	.coil_rd = coil_rd,
	.coil_wr = coil_wr,
	.holding_reg_rd = holding_reg_rd,
	.holding_reg_wr = holding_reg_wr,
	.input_reg_rd = input_reg_rd,
};

#ifdef CONFIG_SETTINGS
#include <zephyr/settings/settings.h>

int mb_handle_get(const char *name, char *val, int val_len_max)
{
	const char *next;
	size_t name_len, offset;

	name_len = settings_name_next(name, &next);

	if (!next) {
		if (!strncmp(name, "history", name_len)) {
			memcpy(val, holding_reg + HOLDING_HIS_SAVE_IDX, val_len_max);
		} else if (!strncmp(name, "rs485_bps", name_len)) {
			memcpy(val, holding_reg + HOLDING_RS485_BPS_IDX, val_len_max);
		} else if (!strncmp(name, "slave_id", name_len)) {
			memcpy(val, holding_reg + HOLDING_SLAVE_ID_IDX, val_len_max);
		} else if (!strncmp(name, "ip", name_len)) {
			memcpy(val, holding_reg + HOLDING_IP_ADDR_1_IDX, val_len_max);
		}
	} else if (!strncmp(name, "ai", name_len)) {
		offset = name_len + 1;
		name_len = settings_name_next(name + offset, &next);
		if (!next) {
			if (!strncmp(name + offset, "enable", name_len)) {
				memcpy(val, holding_reg + HOLDING_AI_EN_IDX, val_len_max);
			} else if (!strncmp(name + offset, "time", name_len)) {
				memcpy(val, holding_reg + HOLDING_AI_SI_IDX, val_len_max);
			}
		}
	} else if (!strncmp(name, "di", name_len)) {
		offset = name_len + 1;
		name_len = settings_name_next(name + offset, &next);
		if (!next) {
			if (!strncmp(name + offset, "enable", name_len)) {
				memcpy(val, holding_reg + HOLDING_DI_EN_IDX, val_len_max);
			} else if (!strncmp(name + offset, "time", name_len)) {
				memcpy(val, holding_reg + HOLDING_DI_SI_IDX, val_len_max);
			}
		}
	} else if (!strncmp(name, "can", name_len)) {
		offset = name_len + 1;
		name_len = settings_name_next(name + offset, &next);
		if (!next) {
			if (!strncmp(name + offset, "id", name_len)) {
				memcpy(val, holding_reg + HOLDING_CAN_ID_IDX, val_len_max);
			} else if (!strncmp(name + offset, "bps", name_len)) {
				memcpy(val, holding_reg + HOLDING_CAN_BPS_IDX, val_len_max);
			}
		}
	} else if (!strncmp(name, "heart", name_len)) {
		offset = name_len + 1;
		name_len = settings_name_next(name + offset, &next);
		if (!next) {
			if (!strncmp(name + offset, "enable", name_len)) {
				memcpy(val, holding_reg + HOLDING_HEART_EN_IDX, val_len_max);
			} else if (!strncmp(name + offset, "time", name_len)) {
				memcpy(val, holding_reg + HOLDING_HEART_TIMEOUT_IDX, val_len_max);
			}
		}
	} else {
		return -ENOENT;
	}

	return 0;
}

int mb_handle_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *next;
	size_t name_len, offset;
	int rc;

	name_len = settings_name_next(name, &next);

	if (!next) {
		if (!strncmp(name, "history", name_len)) {
			rc = read_cb(cb_arg, holding_reg + HOLDING_HIS_SAVE_IDX, sizeof(uint16_t));
			LOG_INF("<modbus/history> = %d", holding_reg[HOLDING_HIS_SAVE_IDX]);
			return 0;
		} else if (!strncmp(name, "rs485_bps", name_len)) {
			rc = read_cb(cb_arg, holding_reg + HOLDING_RS485_BPS_IDX, sizeof(uint16_t));
			LOG_INF("<modbus/rs485_bps> = %d", holding_reg[HOLDING_RS485_BPS_IDX]);
			return 0;
		} else if (!strncmp(name, "slave_id", name_len)) {
			rc = read_cb(cb_arg, holding_reg + HOLDING_SLAVE_ID_IDX, sizeof(uint16_t));
			LOG_INF("<modbus/slave_id> = %d", holding_reg[HOLDING_SLAVE_ID_IDX]);
			return 0;
		} else if (!strncmp(name, "ip", name_len)) {
			rc = read_cb(cb_arg, holding_reg + HOLDING_IP_ADDR_1_IDX,
				     sizeof(uint16_t) * 4);
			LOG_INF("<modbus/ip> = %d.%d.%d.%d", holding_reg[HOLDING_IP_ADDR_1_IDX],
				holding_reg[HOLDING_IP_ADDR_2_IDX],
				holding_reg[HOLDING_IP_ADDR_3_IDX],
				holding_reg[HOLDING_IP_ADDR_4_IDX]);
			return 0;
		}
	} else if (!strncmp(name, "ai", name_len)) {
		offset = name_len + 1;
		name_len = settings_name_next(name + offset, &next);
		if (!next) {
			if (!strncmp(name + offset, "enable", name_len)) {
				rc = read_cb(cb_arg, holding_reg + HOLDING_AI_EN_IDX,
					     sizeof(uint16_t));
				LOG_INF("<modbus/ai/enable> = 0x%x",
					holding_reg[HOLDING_AI_EN_IDX]);
				return 0;
			} else if (!strncmp(name + offset, "time", name_len)) {
				rc = read_cb(cb_arg, holding_reg + HOLDING_AI_SI_IDX,
					     sizeof(uint16_t));
				LOG_INF("<modbus/ai/time> = %d ms", holding_reg[HOLDING_AI_SI_IDX]);
				return 0;
			}
		}
	} else if (!strncmp(name, "di", name_len)) {
		offset = name_len + 1;
		name_len = settings_name_next(name + offset, &next);
		if (!next) {
			if (!strncmp(name + offset, "enable", name_len)) {
				rc = read_cb(cb_arg, holding_reg + HOLDING_DI_EN_IDX,
					     sizeof(uint16_t));
				LOG_INF("<modbus/di/enable> = 0x%x",
					holding_reg[HOLDING_DI_EN_IDX]);
				return 0;
			} else if (!strncmp(name + offset, "time", name_len)) {
				rc = read_cb(cb_arg, holding_reg + HOLDING_DI_SI_IDX,
					     sizeof(uint16_t));
				LOG_INF("<modbus/di/time> = %d ms", holding_reg[HOLDING_DI_SI_IDX]);
				return 0;
			}
		}
	} else if (!strncmp(name, "can", name_len)) {
		offset = name_len + 1;
		name_len = settings_name_next(name + offset, &next);
		if (!next) {
			if (!strncmp(name + offset, "id", name_len)) {
				rc = read_cb(cb_arg, holding_reg + HOLDING_CAN_ID_IDX,
					     sizeof(uint16_t));
				LOG_INF("<modbus/can/id> = %d", holding_reg[HOLDING_CAN_ID_IDX]);
				return 0;
			} else if (!strncmp(name + offset, "bps", name_len)) {
				rc = read_cb(cb_arg, holding_reg + HOLDING_CAN_BPS_IDX,
					     sizeof(uint16_t));
				LOG_INF("<modbus/can/bps> = %d", holding_reg[HOLDING_CAN_BPS_IDX]);
				return 0;
			}
		}
	} else if (!strncmp(name, "heart", name_len)) {
		offset = name_len + 1;
		name_len = settings_name_next(name + offset, &next);
		if (!next) {
			if (!strncmp(name + offset, "enable", name_len)) {
				rc = read_cb(cb_arg, holding_reg + HOLDING_HEART_EN_IDX,
					     sizeof(uint16_t));
				LOG_INF("<modbus/heart/enable> = 0x%x",
					holding_reg[HOLDING_HEART_EN_IDX]);
				return 0;
			} else if (!strncmp(name + offset, "time", name_len)) {
				rc = read_cb(cb_arg, holding_reg + HOLDING_HEART_TIMEOUT_IDX,
					     sizeof(uint16_t));
				LOG_INF("<modbus/heart/time> = %d ms",
					holding_reg[HOLDING_HEART_TIMEOUT_IDX]);
				return 0;
			}
		}
	}

	return -ENOENT;
}

int mb_handle_export(int (*cb)(const char *name, const void *value, size_t val_len))
{
	LOG_INF("export keys under <modbus> handler");
	(void)cb("modbus/ai/enable", holding_reg + HOLDING_AI_EN_IDX, sizeof(uint16_t));
	(void)cb("modbus/ai/time", holding_reg + HOLDING_AI_SI_IDX, sizeof(uint16_t));
	(void)cb("modbus/di/enable", holding_reg + HOLDING_DI_EN_IDX, sizeof(uint16_t));
	(void)cb("modbus/di/time", holding_reg + HOLDING_DI_SI_IDX, sizeof(uint16_t));
	(void)cb("modbus/history", holding_reg + HOLDING_HIS_SAVE_IDX, sizeof(uint16_t));
	(void)cb("modbus/can/id", holding_reg + HOLDING_CAN_ID_IDX, sizeof(uint16_t));
	(void)cb("modbus/can/bps", holding_reg + HOLDING_CAN_BPS_IDX, sizeof(uint16_t));
	(void)cb("modbus/rs485_bps", holding_reg + HOLDING_RS485_BPS_IDX, sizeof(uint16_t));
	(void)cb("modbus/slave_id", holding_reg + HOLDING_SLAVE_ID_IDX, sizeof(uint16_t));
	/* check ip is valid */
	if (get_holding_reg(HOLDING_IP_ADDR_4_IDX) != 0xff &&
	    get_holding_reg(HOLDING_IP_ADDR_4_IDX) != 0 &&
	    !IN_RANGE(get_holding_reg(HOLDING_IP_ADDR_1_IDX), 224, 239)) {
		(void)cb("modbus/ip", holding_reg + HOLDING_IP_ADDR_1_IDX, sizeof(uint16_t) * 4);
	}
	(void)cb("modbus/heart/enable", holding_reg + HOLDING_HEART_EN_IDX, sizeof(uint16_t));
	(void)cb("modbus/heart/time", holding_reg + HOLDING_HEART_TIMEOUT_IDX, sizeof(uint16_t));

	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(modbus, "modbus", mb_handle_get, mb_handle_set, NULL,
			       mb_handle_export);

static int main_settings_init(void)
{
	return settings_subsys_init();
}

SYS_INIT(main_settings_init, APPLICATION, 10);
#endif
