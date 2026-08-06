/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * UDP 业务配置命令处理 (gateway 二进制帧风格).
 *
 * 配置端口 (默认 8600) 由 udp_fw_upgrade 库自管 (RX 线程 + 固件升级命令
 * 0x01-0x05), 本模块仅注册 app_cmd_handler 回调处理业务命令 (0x12-0x21,
 * 即 UDP 固件升级命令之外的 "命令模式参数设置").
 * 所有参数以 holding 寄存器为唯一数据源 (与 Modbus 读写视图一致),
 * 修改后 settings_save() 持久化; 大部分参数重启后生效
 * (RS485 波特率/slave_id/网络模式在 SYS_INIT 时应用).
 *
 * 回复通过 udp_fw_reply() 发送 (库自管 socket + 同子网单播/跨子网广播路由).
 * 响应字节同时供 CAN 侧复用 (can.c 按帧协议分帧发送), 保证 CAN/UDP 语义一致.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/app_version.h>
#include <zephyr/logging/log.h>
#ifdef CONFIG_SETTINGS
#include <zephyr/settings/settings.h>
#endif
#include <data_collect.h>
#include <udp_fw_upgrade.h>
#define DC_NO_MODBUS_LOG_MODULE
#include "init.h"

LOG_MODULE_REGISTER(dc_udp_cmd, LOG_LEVEL_INF);

/* ================================================================
 * 配置命令执行 + 响应构建 (UDP/CAN 共用)
 *
 * 对 SET_* 命令先施加副作用 (写 holding 寄存器 + settings_save),
 * 再生成 GET_* 风格的响应字节到 out; 返回响应长度, 未知命令返回 -1.
 * 输出最大 8B (GET_INFO), CAN 侧按 ≤6B/帧分帧发送.
 * ================================================================ */
int dc_build_config_payload(uint8_t cmd, const uint8_t *req, size_t req_len,
			    uint8_t *out, size_t out_len)
{
	bool saved = false;
	int rlen = -1;

	switch (cmd) {
	/* ============ 网络: IP + Modbus TCP 端口 ============ */
	case UDP_CMD_SET_NET: {
		/* [ip 4B][tcp_port 2B BE] = 6B.
		 * DHCP 模式: 忽略 ip 字段 (IP 由 DHCP 分配), 只写 tcp_port. */
		if (req_len >= 6) {
			if (get_holding_reg(HOLDING_NET_MODE_IDX) == DC_NET_MODE_STATIC) {
				update_holding_reg(HOLDING_IP_ADDR_1_IDX, req[0]);
				update_holding_reg(HOLDING_IP_ADDR_2_IDX, req[1]);
				update_holding_reg(HOLDING_IP_ADDR_3_IDX, req[2]);
				update_holding_reg(HOLDING_IP_ADDR_4_IDX, req[3]);
			}
			update_holding_reg(HOLDING_TCP_PORT_IDX, sys_get_be16(req + 4));
			saved = true;
			LOG_INF("set net: ip=%d.%d.%d.%d tcp_port=%d mode=%d",
				get_holding_reg(HOLDING_IP_ADDR_1_IDX),
				get_holding_reg(HOLDING_IP_ADDR_2_IDX),
				get_holding_reg(HOLDING_IP_ADDR_3_IDX),
				get_holding_reg(HOLDING_IP_ADDR_4_IDX),
				get_holding_reg(HOLDING_TCP_PORT_IDX),
				get_holding_reg(HOLDING_NET_MODE_IDX));
		}
		__fallthrough;
	}
	case UDP_CMD_GET_NET: {
		/* → [ip 4B][tcp_port 2B BE] = 6B. IP 取 live interface (DHCP 实际地址). */
		if (out_len >= 6) {
			struct in_addr *live = dc_get_live_ipv4();

			if (live) {
				memcpy(out, &live->s_addr, 4);
			} else {
				out[0] = get_holding_reg(HOLDING_IP_ADDR_1_IDX);
				out[1] = get_holding_reg(HOLDING_IP_ADDR_2_IDX);
				out[2] = get_holding_reg(HOLDING_IP_ADDR_3_IDX);
				out[3] = get_holding_reg(HOLDING_IP_ADDR_4_IDX);
			}
			sys_put_be16(get_holding_reg(HOLDING_TCP_PORT_IDX), out + 4);
			rlen = 6;
		}
		break;
	}

	case UDP_CMD_SET_NET_MODE: {
		/* [mode 1B] (0=静态,1=DHCP), 重启生效 */
		if (req_len >= 1 && req[0] <= 1) {
			update_holding_reg(HOLDING_NET_MODE_IDX, req[0]);
			saved = true;
			LOG_INF("set net mode: %s", req[0] ? "DHCP" : "static");
		}
		__fallthrough;
	}
	case UDP_CMD_GET_NET_MODE: {
		/* → [mode 1B] */
		if (out_len >= 1) {
			out[0] = get_holding_reg(HOLDING_NET_MODE_IDX);
			rlen = 1;
		}
		break;
	}

	/* ============ Modbus RTU 运行参数 ============ */
	case UDP_CMD_SET_MODBUS: {
		/* [slave_id 2B BE][rs485_bps 4B BE] = 6B (重启后 rtu/tcp 生效) */
		if (req_len >= 6) {
			uint16_t slave_id = sys_get_be16(req);
			uint32_t bps = sys_get_be32(req + 2);

			if (bps > 0 && bps <= 0xffff) {
				update_holding_reg(HOLDING_RS485_BPS_IDX, bps);
				saved = true;
			}
			if (slave_id > 0 && slave_id < 0x100) {
				update_holding_reg(HOLDING_SLAVE_ID_IDX, slave_id);
				saved = true;
			}
			LOG_INF("set modbus: slave_id=%d rs485_bps=%d",
				get_holding_reg(HOLDING_SLAVE_ID_IDX),
				get_holding_reg(HOLDING_RS485_BPS_IDX));
		}
		__fallthrough;
	}
	case UDP_CMD_GET_MODBUS: {
		/* → [slave_id 2B BE][rs485_bps 4B BE] = 6B */
		if (out_len >= 6) {
			sys_put_be16(get_holding_reg(HOLDING_SLAVE_ID_IDX), out);
			sys_put_be32(get_holding_reg(HOLDING_RS485_BPS_IDX), out + 2);
			rlen = 6;
		}
		break;
	}

	/* ============ 采样间隔 + 历史开关 ============ */
	case UDP_CMD_SET_SAMPLE: {
		/* [di_si 2B BE][ai_si 2B BE][his_en 1B] = 5B */
		if (req_len >= 5) {
			update_holding_reg(HOLDING_DI_SI_IDX, sys_get_be16(req));
			update_holding_reg(HOLDING_AI_SI_IDX, sys_get_be16(req + 2));
			update_holding_reg(HOLDING_HIS_SAVE_IDX, req[4]);
			history_enable_write(!!req[4]);
			saved = true;
			LOG_INF("set sample: di_si=%d ai_si=%d his=%d",
				get_holding_reg(HOLDING_DI_SI_IDX),
				get_holding_reg(HOLDING_AI_SI_IDX),
				get_holding_reg(HOLDING_HIS_SAVE_IDX));
		}
		__fallthrough;
	}
	case UDP_CMD_GET_SAMPLE: {
		/* → [di_si 2B BE][ai_si 2B BE][his_en 1B] = 5B */
		if (out_len >= 5) {
			sys_put_be16(get_holding_reg(HOLDING_DI_SI_IDX), out);
			sys_put_be16(get_holding_reg(HOLDING_AI_SI_IDX), out + 2);
			out[4] = get_holding_reg(HOLDING_HIS_SAVE_IDX);
			rlen = 5;
		}
		break;
	}

	/* ============ CAN 配置 (信息性, 实际波特率来自 Kconfig) ============ */
	case UDP_CMD_SET_CAN: {
		/* [can_id 2B BE][can_bps 2B BE] = 4B */
		if (req_len >= 4) {
			update_holding_reg(HOLDING_CAN_ID_IDX, sys_get_be16(req));
			update_holding_reg(HOLDING_CAN_BPS_IDX, sys_get_be16(req + 2));
			saved = true;
			LOG_INF("set can: id=0x%03x bps=%d",
				get_holding_reg(HOLDING_CAN_ID_IDX),
				get_holding_reg(HOLDING_CAN_BPS_IDX));
		}
		__fallthrough;
	}
	case UDP_CMD_GET_CAN: {
		/* → [can_id 2B BE][can_bps 2B BE] = 4B */
		if (out_len >= 4) {
			sys_put_be16(get_holding_reg(HOLDING_CAN_ID_IDX), out);
			sys_put_be16(get_holding_reg(HOLDING_CAN_BPS_IDX), out + 2);
			rlen = 4;
		}
		break;
	}

	/* ============ 心跳 (Modbus DO 清出超时) ============ */
	case UDP_CMD_SET_HEART: {
		/* [heart_en 1B][heart_timeout 2B BE] = 3B */
		if (req_len >= 3) {
			update_holding_reg(HOLDING_HEART_EN_IDX, req[0] ? 1 : 0);
			update_holding_reg(HOLDING_HEART_TIMEOUT_IDX,
					   MAX(sys_get_be16(req + 1), 500));
			saved = true;
			LOG_INF("set heart: en=%d timeout=%d",
				get_holding_reg(HOLDING_HEART_EN_IDX),
				get_holding_reg(HOLDING_HEART_TIMEOUT_IDX));
		}
		__fallthrough;
	}
	case UDP_CMD_GET_HEART: {
		/* → [heart_en 1B][heart_timeout 2B BE] = 3B */
		if (out_len >= 3) {
			out[0] = get_holding_reg(HOLDING_HEART_EN_IDX);
			sys_put_be16(get_holding_reg(HOLDING_HEART_TIMEOUT_IDX), out + 1);
			rlen = 3;
		}
		break;
	}

	/* ============ 校时 ============ */
	case UDP_CMD_SET_TIME: {
		/* [timestamp 4B BE] → 回显设置后的时间 (立即生效) */
		uint32_t t = 0;
		time_t now = time(NULL);

		if (req_len >= 4) {
			t = sys_get_be32(req);
		}
		if (t != 0) {
			set_timestamp((time_t)t);
			update_holding_reg(HOLDING_TIMESTAMPH_IDX, t >> 16);
			update_holding_reg(HOLDING_TIMESTAMPL_IDX, t & UINT16_MAX);
			now = (time_t)t;
		}
		if (out_len >= 4) {
			sys_put_be32((uint32_t)now, out);
			rlen = 4;
		}
		break;
	}

	/* ============ 设备信息 ============ */
	case UDP_CMD_GET_INFO: {
		/* → [version 4B BE][uptime_s 4B BE] = 8B */
		if (out_len >= 8) {
			sys_put_be32(APPVERSION, out);
			sys_put_be32((uint32_t)(k_uptime_get() / 1000), out + 4);
			rlen = 8;
		}
		break;
	}

	default:
		break;
	}

	if (saved) {
#ifdef CONFIG_SETTINGS
		settings_save();
#endif
	}
	return rlen;
}

/* ================================================================
 * UDP 应用命令回调 (由 udp_fw_upgrade 库 RX 线程调用, 不可长时间阻塞)
 * ================================================================ */
static bool app_cmd_handler(uint8_t cmd, const uint8_t *cmd_data, size_t cmd_len,
			    void *user_data)
{
	ARG_UNUSED(user_data);
	uint8_t resp[8];
	int rlen = dc_build_config_payload(cmd, cmd_data, cmd_len, resp, sizeof(resp));

	if (rlen < 0) {
		return false;
	}
	udp_fw_reply(cmd, resp, rlen);
	return true;
}

/* ================================================================
 * 初始化: 注册配置命令回调 (固件升级库 SYS_INIT 自管配置端口 socket)
 * ================================================================ */
static int dc_udp_cmd_init(void)
{
	udp_fw_set_app_handler(app_cmd_handler, NULL);
	return 0;
}

SYS_INIT(dc_udp_cmd_init, APPLICATION, CONFIG_DC_UDP_CMD_INIT_PRIORITY);
