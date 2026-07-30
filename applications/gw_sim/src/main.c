/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * gw_sim 主入口 - gateway 网络代码的 native_sim 移植版
 * nsos 模式: socket 走主机网络 (NET_SOCKETS_OFFLOAD), 无需 net_if_up/链路等待.
 * UDP 双端口/配置命令/固件升级 由 udp.c 的接收线程处理 (nsos socket).
 */

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <gateway.h>
#include <zephyr/app_version.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(gw_sim, LOG_LEVEL_INF);

gateway_params_t gw_params;

int main(void)
{
	/* native_sim stdout 默认全缓冲, 重定向到文件时日志不即时 flush. 改行缓冲. */
	setvbuf(stdout, NULL, _IOLBF, 0);

	LOG_INF("build time: %s-%s", __DATE__, __TIME__);
	LOG_INF("board: %s, version: %s", CONFIG_BOARD_TARGET, APP_VERSION_STRING);

	/* 初始化默认配置 (无持久化) */
	gw_params.rf24_channel = RF24_DEFAULT_CH;
	memset(gw_params.rf24_addr, 0, RF24_ADDR_LEN);
	strncpy(gw_params.ip_addr, GATEWAY_DEFAULT_IP, sizeof(gw_params.ip_addr) - 1);
	strncpy(gw_params.netmask, GATEWAY_DEFAULT_MASK, sizeof(gw_params.netmask) - 1);
	strncpy(gw_params.gateway, GATEWAY_DEFAULT_GW, sizeof(gw_params.gateway) - 1);
	gw_params.data_port = GATEWAY_DATA_PORT_DEFAULT;

	/* 初始化各模块 (nRF24 stub) */
	gw_rf24_init();

	/* nsos 模式: socket 由 udp.c 接收线程经主机网络创建, 主线程无需 net_if 操作 */
	LOG_INF("Network config: %s/%s gw %s (nsos host networking)",
		gw_params.ip_addr, gw_params.netmask, gw_params.gateway);
	gw_params.running = true;
	LOG_INF("gw_sim ready");

	while (1) {
		k_sleep(K_MSEC(1000));
	}

	return 0;
}
