/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * gateway UDP 测试应用 — 主入口
 * 初始化静态 IP, 启动 UDP 双端口线程 (线程在 udp.c 中 K_THREAD_DEFINE)
 *
 * 平移自 gateway/src/main.c, 去掉 swd_recover (F1 专属) 和 RF24 初始化.
 *
 * 网络就绪同步: STM32F407 内置 MAC 接口注册晚于 main 启动, 故先注册
 * NET_EVENT_IF_UP 回调, 轮询等待接口出现后配置静态 IP 并 net_if_up, 再用
 * 信号量等接口 oper state UP. 这替代原先固定 k_msleep(500) 的盲等待.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/app_version.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <gateway_udp_test.h>
#ifndef CONFIG_FLASH_SIZE
#define CONFIG_FLASH_SIZE 0x1000
#endif


#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(gut_main, LOG_LEVEL_INF);

gut_params_t gut_params;

/* ================================================================
 * 网络链路就绪同步
 * ================================================================
 * 内置 MAC 驱动在 POST_KERNEL 阶段注册网络接口并初始化 PHY; PHY 检测到载波
 * 后 net_eth_carrier_on → oper state UP → notify_iface_up 发出 NET_EVENT_IF_UP.
 * 用信号量捕获该事件, 通知 UDP 线程接口已可用. */
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
 * 网络初始化 (静态 IP)
 * ================================================================
 * 调用前提: iface 已注册 (由 main 轮询等待). 先注册 NET_EVENT_IF_UP 回调
 * 再 net_if_up, 保证不丢事件 (carrier 已在线时 net_if_up 同步生成事件). */
static int net_init(struct net_if *iface)
{
	struct in_addr addr, mask, gw;

	/* 先注册链路事件回调, 再 net_if_up */
	net_mgmt_init_event_callback(&net_mgmt_cb, net_mgmt_handler, NET_EVENT_IF_UP);
	net_mgmt_add_event_callback(&net_mgmt_cb);

	if (net_addr_pton(AF_INET, gut_params.ip_addr, &addr) < 0) {
		LOG_ERR("Invalid IP address: %s", gut_params.ip_addr);
		return -EINVAL;
	}
	if (net_addr_pton(AF_INET, gut_params.netmask, &mask) < 0) {
		LOG_ERR("Invalid netmask: %s", gut_params.netmask);
		return -EINVAL;
	}
	if (net_addr_pton(AF_INET, gut_params.gateway, &gw) < 0) {
		LOG_ERR("Invalid gateway: %s", gut_params.gateway);
		return -EINVAL;
	}

	net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0);
	net_if_ipv4_set_netmask_by_addr(iface, &addr, &mask);
	net_if_ipv4_set_gw(iface, &gw);

	/* 触发接口 administrative up: 幂等. oper state 由 (admin up + carrier)
	 * 决定, 是 NET_EVENT_IF_UP 的可靠触发点. */
	net_if_up(iface);

	LOG_INF("Network: %s/%s gw %s", gut_params.ip_addr, gut_params.netmask,
		gut_params.gateway);
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
	gut_params.rf24_channel = RF24_DEFAULT_CH;
	memset(gut_params.rf24_addr, 0, RF24_ADDR_LEN);
	strncpy(gut_params.ip_addr, GUT_DEFAULT_IP, sizeof(gut_params.ip_addr) - 1);
	strncpy(gut_params.netmask, GUT_DEFAULT_MASK, sizeof(gut_params.netmask) - 1);
	strncpy(gut_params.gateway, GUT_DEFAULT_GW, sizeof(gut_params.gateway) - 1);
	gut_params.data_port = GUT_DATA_PORT_DEFAULT;
	gut_params.echo = false;
	k_event_init(&gut_params.event);

	/* 加载持久化配置 (覆盖默认值) */
	settings_load();

	/* 等待内置 MAC 接口注册 (POST_KERNEL 驱动 init, 晚于 main 启动).
	 * 轮询 net_if_get_default 直到接口出现, 带超时兜底避免永久阻塞. */
	struct net_if *iface = NULL;

	for (int i = 0; i < 100; i++) {
		iface = net_if_get_default();
		if (iface != NULL) {
			break;
		}
		k_msleep(50);
	}
	if (iface == NULL) {
		LOG_ERR("No network interface found (检查 ETH 驱动/PHY)");
		/* 不 set event: UDP 线程持续等待, 避免在无接口状态下 bind 报错刷屏 */
	} else {
		int ret = net_init(iface);

		if (ret != 0) {
			LOG_ERR("net_init failed: %d", ret);
		} else {
			/* 等 PHY 链路 up: 若注册回调时 oper state 已 UP 则跳过;
			 * 否则等 NET_EVENT_IF_UP 信号量, 带超时兜底 (网线未插时不永久阻塞,
			 * UDP 线程链路恢复后自然恢复转发). */
			if (net_if_oper_state(iface) != NET_IF_OPER_UP) {
				if (k_sem_take(&net_link_sem, K_SECONDS(5)) != 0) {
					LOG_WRN("net link up timeout, continue anyway");
				}
			}
			/* 唤醒 UDP 线程, 它们此刻才开始创建 socket 并 bind */
			k_event_set(&gut_params.event, 0x1);
		}
	}

	gut_params.running = true;
	LOG_INF("gateway_udp_test ready (data:%d config:%d)", gut_params.data_port,
		GUT_CONFIG_PORT);

	while (1) {
		k_sleep(K_MSEC(1000));
	}

	return 0;
}
