/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Gateway 主入口 - 数据中转网关
 * 接收 mod_handler 的 nRF24 数据，通过 W5500 UDP 转发给上位机
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/settings/settings.h>
#include <gateway.h>
#include <zephyr/app_version.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#ifndef CONFIG_FLASH_SIZE
#define CONFIG_FLASH_SIZE 0x1000
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(gateway, LOG_LEVEL_INF);

gateway_params_t gw_params;

/* ================================================================
 * SWD 恢复: SPI3 引脚 (PB3/PB4/PA15) 复用 JTAG 引脚, Zephyr pinctrl 在
 * 应用 SPI3_REMAP0 时把 AFIO_MAPR.SWJ_CFG 设成 111 (JTAG+SWD 全关) 以释放
 * 这些引脚。但 SWJ_CFG=111 也会关闭 SWD, 导致 ST-Link 无法再通过 SWD 烧写。
 * 此处在所有驱动初始化 (POST_KERNEL) 之后, 把 SWJ_CFG 改回 010
 * (AFIO_MAPR_SWJ_CFG_JTAGDISABLE = 0x02000000): 关 JTAG 保留 SWD, 既不
 * 影响已配置的 SPI3 引脚, 又恢复 SWD 烧写能力。
 * ================================================================ */
static int swd_recover(void)
{
	uint32_t mapr = AFIO->MAPR & ~AFIO_MAPR_SWJ_CFG;

	AFIO->MAPR = mapr | AFIO_MAPR_SWJ_CFG_JTAGDISABLE;
	return 0;
}
SYS_INIT(swd_recover, PRE_KERNEL_2, 1);

/* ================================================================
 * 网络链路就绪事件
 * ================================================================
 * W5500 驱动在 PHY 检测到载波 (PHYCFGR.LNK) 后调 net_eth_carrier_on →
 * net_if_carrier_on (置 NET_IF_LOWER_UP) → update_operational_state: 接口
 * admin up 且 carrier ok 时 oper state → UP → notify_iface_up 发出
 * NET_EVENT_IF_UP. 此处用信号量捕获该事件, 替代原先固定 k_msleep(500) 的盲
 * 等待 (PHY 自动协商耗时不定, 网线未插时延时更毫无意义).
 * ================================================================ */
static K_SEM_DEFINE(net_link_sem, 0, 1);
static struct net_mgmt_event_callback net_mgmt_cb;

static void net_mgmt_handler(struct net_mgmt_event_callback *cb,
			     uint64_t mgmt_event, struct net_if *iface)
{
	if (mgmt_event == NET_EVENT_IF_UP) {
		LOG_INF("net link up");
		k_sem_give(&net_link_sem);
	}
}

/* ================================================================
 * 网络初始化 (W5500 静态 IP)
 * ================================================================ */
static int net_init(void)
{
	struct net_if *iface = net_if_get_default();
	struct in_addr addr, mask, gw;

	if (!iface) {
		LOG_ERR("No network interface found");
		return -ENODEV;
	}

	/* 先注册链路事件回调, 再 net_if_up: 若 carrier 此刻已在线, net_if_up 会
	 * 同步生成 NET_EVENT_IF_UP (异步投递至此回调); 若未在线, 等 W5500 检测到
	 * 载波后生成. 注册顺序保证不丢事件. */
	net_mgmt_init_event_callback(&net_mgmt_cb, net_mgmt_handler, NET_EVENT_IF_UP);
	net_mgmt_add_event_callback(&net_mgmt_cb);

	if (net_addr_pton(AF_INET, gw_params.ip_addr, &addr) < 0) {
		LOG_ERR("Invalid IP address: %s", gw_params.ip_addr);
		return -EINVAL;
	}

	/* 掩码固定 /24; 网关 = IP 末段改 1 (a.b.c.1), 均不存储, 运行时派生 */
	mask.s_addr = htonl(0xFFFFFF00);
	memcpy(&gw, &addr, sizeof(gw));
	((uint8_t *)&gw.s_addr)[3] = 1;

	net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0);
	net_if_ipv4_set_netmask_by_addr(iface, &addr, &mask);
	net_if_ipv4_set_gw(iface, &gw);

	/* 触发接口 administrative up: 幂等 (已 up 返回 -EALREADY 无害). 这是
	 * NET_EVENT_IF_UP 的可靠触发点 — oper state 由 (admin up + carrier) 决定. */
	net_if_up(iface);

	char gw_str[NET_IPV4_ADDR_LEN];

	net_addr_ntop(AF_INET, &gw, gw_str, sizeof(gw_str));
	LOG_INF("Network: %s/24 gw %s", gw_params.ip_addr, gw_str);
	return 0;
}

int main(void)
{
	LOG_INF("build time: %s-%s", __DATE__, __TIME__);
	LOG_INF("board: %s, system clk: %dMHz", CONFIG_BOARD_TARGET,
		CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / MHZ(1));
	LOG_INF("flash size: %dKB, ram size: %dKB", CONFIG_FLASH_SIZE, CONFIG_SRAM_SIZE);
	LOG_INF("version: %s", APP_VERSION_STRING);

	/* 初始化默认配置 */
	gw_params.rf24_channel = RF24_DEFAULT_CH;
	gw_params.rf24_addr[0] = 0;
	gw_params.rf24_addr[1] = 0;
	gw_params.rf24_addr[2] = 0;
	gw_params.rf24_addr[3] = 0;
	gw_params.rf24_addr[4] = 0;
	strncpy(gw_params.ip_addr, GATEWAY_DEFAULT_IP, sizeof(gw_params.ip_addr) - 1);
	gw_params.data_port = GATEWAY_DATA_PORT_DEFAULT;

	/* 加载持久化配置 (覆盖默认值) */
	gw_config_load();

	/* 初始化各模块 */
	gw_rf24_init();

	net_init();

	/* 等待 PHY 链路 up: 若 net_init() 注册回调时链路已 up (oper state 已 UP)
	 * 则直接跳过; 否则等 NET_EVENT_IF_UP 信号量. 带超时兜底, 避免网线未插时
	 * 永久阻塞 (UDP 收发线程独立运行, 链路恢复后自然恢复转发). */
	struct net_if *iface = net_if_get_default();

	if (iface != NULL && net_if_oper_state(iface) != NET_IF_OPER_UP) {
		if (k_sem_take(&net_link_sem, K_SECONDS(5)) != 0) {
			LOG_WRN("net link up timeout, continue anyway");
		}
	}

	gw_params.running = true;
	LOG_INF("Gateway ready");

	while (1) {
		k_sleep(K_MSEC(1000));
	}

	return 0;
}
