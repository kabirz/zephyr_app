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
SYS_INIT(swd_recover, APPLICATION, 0);

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

	if (net_addr_pton(AF_INET, gw_params.ip_addr, &addr) < 0) {
		LOG_ERR("Invalid IP address: %s", gw_params.ip_addr);
		return -EINVAL;
	}
	if (net_addr_pton(AF_INET, gw_params.netmask, &mask) < 0) {
		LOG_ERR("Invalid netmask: %s", gw_params.netmask);
		return -EINVAL;
	}
	if (net_addr_pton(AF_INET, gw_params.gateway, &gw) < 0) {
		LOG_ERR("Invalid gateway: %s", gw_params.gateway);
		return -EINVAL;
	}

	net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0);
	net_if_ipv4_set_netmask_by_addr(iface, &addr, &mask);
	net_if_ipv4_set_gw(iface, &gw);

	LOG_INF("Network: %s/%s gw %s", gw_params.ip_addr, gw_params.netmask, gw_params.gateway);
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
	strncpy(gw_params.netmask, GATEWAY_DEFAULT_MASK, sizeof(gw_params.netmask) - 1);
	strncpy(gw_params.gateway, GATEWAY_DEFAULT_GW, sizeof(gw_params.gateway) - 1);
	gw_params.udp_port = GATEWAY_DEFAULT_UDP_PORT;

	/* 加载持久化配置 (覆盖默认值) */
	gw_config_load();

	/* 初始化各模块 */
	gw_rf24_init();

	/* 等待网络就绪后初始化 */
	k_msleep(500);
	net_init();

	gw_params.running = true;
	LOG_INF("Gateway ready");

	while (1) {
		k_sleep(K_MSEC(1000));
	}

	return 0;
}
