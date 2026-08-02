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
#include <zephyr/net/dhcpv4.h>
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

/* 网络事件回调 (静态注册, 编译时进 section, 不受运行时注册时序影响).
 * 注意: Zephyr net_mgmt 的 mask 匹配按 layer 精确相等, 不同 layer 的事件
 * (IF_UP=L2, IPV4_ADDR_ADD=L3) 不能 OR 在同一个 mask 里, 否则全部匹配失败.
 * 故分成两个独立 handler 注册. */

/* IF_UP: carrier 就绪 → 唤醒 main + (DHCP 模式) 启动 DHCP 客户端. */
static void net_if_event_handler(uint64_t mgmt_event, struct net_if *iface,
				 void *info, size_t info_length, void *user_data)
{
	ARG_UNUSED(info);
	ARG_UNUSED(info_length);
	ARG_UNUSED(user_data);

	if (mgmt_event != NET_EVENT_IF_UP) {
		return;
	}
	LOG_INF("net link up");
	k_sem_give(&net_link_sem);
	/* DHCP 模式: carrier up 后才启动 DHCP 客户端 (启动早了 Discover 发不出) */
	if (gut_params.use_dhcp) {
		LOG_INF("Starting DHCPv4 client...");
		net_dhcpv4_start(iface);
	}
}

/* ADDR_ADD: IPv4 地址分配完成 → 打印 IP/掩码/网关 (DHCP 模式额外打印租期). */
static void net_ipv4_event_handler(uint64_t mgmt_event, struct net_if *iface,
				   void *info, size_t info_length, void *user_data)
{
	ARG_UNUSED(info);
	ARG_UNUSED(info_length);
	ARG_UNUSED(user_data);

	if (mgmt_event != NET_EVENT_IPV4_ADDR_ADD) {
		return;
	}
	for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
		if (iface->config.ip.ipv4->unicast[i].ipv4.addr_type !=
		    NET_ADDR_DHCP && iface->config.ip.ipv4->unicast[i].ipv4.addr_type !=
		    NET_ADDR_MANUAL) {
			continue;
		}
		char buf[NET_IPV4_ADDR_LEN];

		net_addr_ntop(AF_INET,
			      &iface->config.ip.ipv4->unicast[i].ipv4.address.in_addr,
			      buf, sizeof(buf));
		LOG_INF("IPv4 address: %s", buf);
		net_addr_ntop(AF_INET, &iface->config.ip.ipv4->unicast[i].netmask,
			      buf, sizeof(buf));
		LOG_INF("IPv4 netmask: %s", buf);
		net_addr_ntop(AF_INET, &iface->config.ip.ipv4->gw, buf, sizeof(buf));
		LOG_INF("IPv4 gateway: %s", buf);
		if (iface->config.ip.ipv4->unicast[i].ipv4.addr_type == NET_ADDR_DHCP) {
			LOG_INF("DHCP lease time: %u seconds", iface->config.dhcpv4.lease_time);
		}
		break;
	}
}

NET_MGMT_REGISTER_EVENT_HANDLER(net_if_handler_cb, NET_EVENT_IF_UP,
				net_if_event_handler, NULL);
NET_MGMT_REGISTER_EVENT_HANDLER(net_ipv4_handler_cb, NET_EVENT_IPV4_ADDR_ADD,
				net_ipv4_event_handler, NULL);

/* ================================================================
 * 网络初始化 (静态 IP / DHCP)
 * ================================================================
 * 调用前提: iface 已注册 (由 main 轮询等待). 事件回调已静态注册 (编译时).
 * DHCP 模式下 net_dhcpv4_start 延迟到 NET_EVENT_IF_UP 回调里调用. */
static int net_init(struct net_if *iface)
{
	if (gut_params.use_dhcp) {
		/* DHCP 模式: 只 up 接口, DHCP 客户端在 IF_UP 回调里启动 */
		LOG_INF("DHCP mode, waiting for link up...");
		net_if_up(iface);
	} else {
		/* 静态模式: 掩码固定 /24; 网关 = IP 末段改 1 (a.b.c.1), 不存储 */
		struct in_addr addr, mask, gw;

		if (net_addr_pton(AF_INET, gut_params.ip_addr, &addr) < 0) {
			LOG_ERR("Invalid IP address: %s", gut_params.ip_addr);
			return -EINVAL;
		}
		mask.s_addr = htonl(0xFFFFFF00);
		memcpy(&gw, &addr, sizeof(gw));
		((uint8_t *)&gw.s_addr)[3] = 1;

		net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0);
		net_if_ipv4_set_netmask_by_addr(iface, &addr, &mask);
		net_if_ipv4_set_gw(iface, &gw);

		net_if_up(iface);

		char gw_str[NET_IPV4_ADDR_LEN];
		net_addr_ntop(AF_INET, &gw, gw_str, sizeof(gw_str));
		LOG_INF("Network: %s/24 gw %s", gut_params.ip_addr, gw_str);
	}
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
	gut_params.data_port = GUT_DATA_PORT_DEFAULT;
	gut_params.use_dhcp = GUT_USE_DHCP_DEFAULT;
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
		LOG_ERR("No network interface found (check ETH driver/PHY)");
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
