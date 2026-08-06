/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * data_collect — 公共定义 (协议契约)
 *
 * 数据采集 (DI / DO / AI) 通过 Modbus RTU (RS485) + Modbus TCP (Raw ADU)
 * 对外提供; 历史数据落盘 littlefs; 参数配置通过 UDP 配置端口 (由
 * udp_fw_upgrade 库自管, 默认 8600) 以 gateway 风格二进制命令帧下发;
 * 固件升级支持 UDP (8600) 与 CAN (0x101-0x105) 双通道, 分别复用
 * libs/udp_fw_upgrade 与 libs/can_fw_upgrade。
 */

#ifndef __DATA_COLLECT_H__
#define __DATA_COLLECT_H__

#include <zephyr/kernel.h>
#include <zephyr/net/net_ip.h>
#include <stdbool.h>
#include <stdint.h>

/* ================================================================
 * UDP 配置命令 (配置端口, 由 udp_fw_upgrade 库 RX 线程分发到 app_cmd_handler).
 * 0x01-0x05 由库内部处理 (FW_START/END/DATA/GET_VERSION/REBOOT), 不会到达此处.
 * 帧格式 [cmd 1B][data...], 与 gateway 上位机协议同构.
 *
 * 网络: 静态模式下掩码固定 255.255.255.0, 网关 = IP 末段改 1 (a.b.c.1),
 *       均不在帧中传输. DHCP 模式下 IP/掩码/网关由 DHCP 服务器分配,
 *       GET_NET 回复 live interface 地址.
 * 端口: 此处 "tcp_port" 指 Modbus TCP 服务端口 (默认 502).
 * ================================================================ */
enum udp_cmd {
	UDP_CMD_SET_NET      = 0x12,  /* [ip 4B][tcp_port 2B BE] = 6B → 回显 6B */
	UDP_CMD_GET_NET      = 0x13,  /* () → [ip 4B][tcp_port 2B BE] = 6B (IP 取 live) */
	UDP_CMD_SET_NET_MODE = 0x16,  /* [mode 1B] (0=静态,1=DHCP) → 回显 1B (重启生效) */
	UDP_CMD_GET_NET_MODE = 0x17,  /* () → [mode 1B] */

	UDP_CMD_SET_MODBUS   = 0x18,  /* [slave_id 2B BE][rs485_bps 4B BE] = 6B → 回显 6B */
	UDP_CMD_GET_MODBUS   = 0x19,  /* () → [slave_id 2B BE][rs485_bps 4B BE] = 6B */

	UDP_CMD_SET_SAMPLE   = 0x1A,  /* [di_si 2B BE][ai_si 2B BE][his_en 1B] = 5B → 回显 5B */
	UDP_CMD_GET_SAMPLE   = 0x1B,  /* () → [di_si 2B BE][ai_si 2B BE][his_en 1B] = 5B */

	UDP_CMD_SET_CAN      = 0x1C,  /* [can_id 2B BE][can_bps 2B BE] = 4B → 回显 4B */
	UDP_CMD_GET_CAN      = 0x1D,  /* () → [can_id 2B BE][can_bps 2B BE] = 4B */

	UDP_CMD_SET_HEART    = 0x1E,  /* [heart_en 1B][heart_timeout 2B BE] = 3B → 回显 3B */
	UDP_CMD_GET_HEART    = 0x1F,  /* () → [heart_en 1B][heart_timeout 2B BE] = 3B */

	UDP_CMD_SET_TIME     = 0x20,  /* [timestamp 4B BE] → 回显 4B (立即生效) */
	UDP_CMD_GET_INFO     = 0x21,  /* () → [version 4B BE][uptime 4B BE] = 8B */
};

/* ================================================================
 * CAN 业务帧
 *   0x101-0x105 由 can_fw_upgrade 库内部处理 (固件升级 + keyhash + 版本字符串)
 *   0x763     DAQ 周期心跳帧 (device → host)
 *   0x1A0     CAN 参数配置命令 (host → device): [sub 1B][payload ≤7B]
 *   0x1A1     CAN 参数配置响应 (device → host): [sub 1B][seq 1B][payload ≤6B]
 * 其中 sub 复用 enum udp_cmd 的取值 (0x12-0x21), 使 CAN/UDP 命令语义一致;
 * 响应 >6B 时按 seq 分帧 (0,1,...), 其余单帧 seq=0。
 * ================================================================ */
#define DC_CAN_HEARTBEAT    0x763
#define DC_CAN_CFG_CMD      0x1A0
#define DC_CAN_CFG_RESP     0x1A1

/* ================================================================
 * 网络默认配置 (参数以 holding 寄存器为准, 默认值见 modbus/init.c)
 * ================================================================ */
#define DC_DEFAULT_TCP_PORT 502     /* Modbus TCP 默认端口 (HOLDING_TCP_PORT_IDX) */
#define DC_NET_MODE_STATIC  0       /* 静态 IP */
#define DC_NET_MODE_DHCP    1

/* ================================================================
 * 网络状态
 * ================================================================ */
/* 网络链路状态 (net.c 的 NET_EVENT_IF_UP/DOWN 回调维护). */
extern volatile bool dc_net_link_up;

/* 接口声明 */

/* net.c: 取本机 live IPv4 地址 (DHCP 分配或静态配置的当前地址), 失败返回 NULL. */
struct in_addr *dc_get_live_ipv4(void);

/* net.c: 等待网络就绪 (接口 + IP 配置完成), 返回后即可创建 socket bind. */
void dc_net_wait_ready(void);

/* udp.c: 执行配置命令 (SET_* 施加副作用 + 生成 GET_* 响应字节).
 * UDP 侧经由 udp_fw_reply() 回复; CAN 侧 (can.c) 按帧协议分帧复用.
 * 返回响应字节数 (0-8), 未知命令返回 -1. */
int dc_build_config_payload(uint8_t cmd, const uint8_t *req, size_t req_len,
			    uint8_t *out, size_t out_len);

#endif /* __DATA_COLLECT_H__ */