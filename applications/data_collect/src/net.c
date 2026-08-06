/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * 网络初始化 (静态 IP / DHCP) + 链路/地址事件管理.
 *
 * IP/端口/网络模式以 holding 寄存器为准 (modbus_init 在 APPLICATION 11
 * 载入默认值并 settings_load, 本线程在所有 SYS_INIT 完成、调度器启动后才
 * 运行, 故寄存器已就绪).
 *
 * 时序: 以太网接口在 boot 阶段注册, 本线程轮询等待接口出现;
 * 静态模式直接配置 IP 上使能; DHCP 模式等接口 oper up 后由 IF_UP 事件回调
 * 启动 DHCP, 再等 IPV4_ADDR_ADD 事件拿到租期地址. 就绪后置 NET_READY_EVENT,
 * 供 Modbus TCP 线程在 bind 前等待.
 */

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_l2.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/logging/log.h>
#include <data_collect.h>
#define DC_NO_MODBUS_LOG_MODULE
#include "modbus/init.h"

LOG_MODULE_REGISTER(dc_net, LOG_LEVEL_INF);

/* 网络就绪事件 (host: modbus TCP/server bind 前等待). 由本线程在网络配置完成后置位. */
#define DC_NET_READY_BIT 0x1
K_EVENT_DEFINE(dc_net_ready_event);

/* 链路状态: IF_UP 置 true, IF_DOWN 置 false. */
volatile bool dc_net_link_up;

static K_SEM_DEFINE(net_link_sem, 0, 1); /* 接口 link up   (IF_UP) */
static K_SEM_DEFINE(net_addr_sem, 0, 1);  /* IPv4 地址分配完成 (IPV4_ADDR_ADD) */

/* ================================================================
 * 网络事件回调 (静态注册, 编译时进 section).
 * ================================================================ */
static void net_if_event_handler(uint64_t mgmt_event, struct net_if *iface,
				 void *info, size_t info_length, void *user_data)
{
	ARG_UNUSED(info);
	ARG_UNUSED(info_length);
	ARG_UNUSED(user_data);

	if (mgmt_event == NET_EVENT_IF_UP) {
		LOG_INF("net link up");
		dc_net_link_up = true;
		k_sem_give(&net_link_sem);
		/* DHCP 模式: carrier up 后才启动 DHCP 客户端 (启动早了 Discover 发不出) */
		if (get_holding_reg(HOLDING_NET_MODE_IDX) == DC_NET_MODE_DHCP) {
			LOG_INF("Starting DHCPv4 client...");
			net_dhcpv4_start(iface);
		}
	} else if (mgmt_event == NET_EVENT_IF_DOWN) {
		LOG_WRN("net link down");
		dc_net_link_up = false;
	}
}

static void net_ipv4_event_handler(uint64_t mgmt, struct net_if *iface,
				   void *info, size_t info_length, void *user_data)
{
	ARG_UNUSED(info);
	ARG_UNUSED(info_length);
	ARG_UNUSED(user_data);

	if (mgmt != NET_EVENT_IPV4_ADDR_ADD) {
		return;
	}
	for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
		if (iface->config.ip.ipv4->unicast[i].ipv4.addr_type != NET_ADDR_DHCP &&
		    iface->config.ip.ipv4->unicast[i].ipv4.addr_type != NET_ADDR_MANUAL) {
			continue;
		}
		char buf[NET_IPV4_ADDR_LEN];

		net_addr_ntop(AF_INET,
			      &iface->config.ip.ipv4->unicast[i].ipv4.address.in_addr,
			      buf, sizeof(buf));
		LOG_INF("IPv4 address: %s", buf);
		k_sem_give(&net_addr_sem);
		return;
	}
}

NET_MGMT_REGISTER_EVENT_HANDLER(dc_net_if_handler_cb, NET_EVENT_IF_UP | NET_EVENT_IF_DOWN,
				net_if_event_handler, NULL);
NET_MGMT_REGISTER_EVENT_HANDLER(dc_net_ipv4_handler_cb, NET_EVENT_IPV4_ADDR_ADD,
				net_ipv4_event_handler, NULL);

/* ================================================================
 * 网络接口/地址
 * ================================================================ */
struct in_addr *dc_get_live_ipv4(void)
{
	struct net_if *iface = net_if_get_first_by_type(&NET_L2_GET_NAME(ETHERNET));

	if (!iface) {
		return NULL;
	}
	return (struct in_addr *)net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);
}

void dc_net_wait_ready(void)
{
	k_event_wait(&dc_net_ready_event, DC_NET_READY_BIT, false, K_FOREVER);
}

/* 等待以太网接口注册 (boot 阶段完成, 轮询兜底) */
static struct net_if *wait_for_eth_iface(void)
{
	for (int i = 0; i < 100; i++) {
		struct net_if *iface = net_if_get_first_by_type(&NET_L2_GET_NAME(ETHERNET));

		if (iface != NULL) {
			return iface;
		}
		k_msleep(50);
	}
	return NULL;
}

/* 静态模式: 掩码固定 /24; 网关 = IP 末段改 1 (a.b.c.1), 不存储 */
static int net_setup_static(struct net_if *iface)
{
	char ip_str[NET_IPV4_ADDR_LEN];
	struct in_addr addr, mask, gw;

	addr.s_addr = 0;
	addr.s4_addr[0] = get_holding_reg(HOLDING_IP_ADDR_1_IDX);
	addr.s4_addr[1] = get_holding_reg(HOLDING_IP_ADDR_2_IDX);
	addr.s4_addr[2] = get_holding_reg(HOLDING_IP_ADDR_3_IDX);
	addr.s4_addr[3] = get_holding_reg(HOLDING_IP_ADDR_4_IDX);

	mask.s_addr = htonl(0xFFFFFF00);
	memcpy(&gw, &addr, sizeof(gw));
	((uint8_t *)&gw.s_addr)[3] = 1;

	if (net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0) == NULL) {
		LOG_ERR("cannot add ip to interface");
		return -1;
	}
	net_if_ipv4_set_netmask_by_addr(iface, &addr, &mask);
	net_if_ipv4_set_gw(iface, &gw);

	net_if_up(iface);

	net_addr_ntop(AF_INET, &addr, ip_str, sizeof(ip_str));
	LOG_INF("static net: %s/24 gw=...", ip_str);
	return 0;
}

static void dc_net_thread(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	struct net_if *iface = wait_for_eth_iface();

	if (!iface) {
		LOG_ERR("no ethernet interface found");
		return;
	}

	bool dhcp = (get_holding_reg(HOLDING_NET_MODE_IDX) == DC_NET_MODE_DHCP);
	int ret = dhcp ? 0 : net_setup_static(iface);

	if (ret == 0) {
		if (net_if_oper_state(iface) != NET_IF_OPER_UP) {
			if (k_sem_take(&net_link_sem, K_SECONDS(5)) != 0) {
				LOG_WRN("net link up timeout, continue anyway");
			}
		}
		if (dhcp) {
			/* DHCP 租约由 IF_UP 回调后的 DHCP 客户端获取, 等 ADDR_ADD */
			if (k_sem_take(&net_addr_sem, K_SECONDS(10)) != 0) {
				LOG_WRN("dhcp addr timeout");
			}
		}
	}

	k_event_set(&dc_net_ready_event, DC_NET_READY_BIT);
	LOG_INF("net ready");

	while (1) {
		k_sleep(K_SECONDS(1));
	}
}

K_THREAD_DEFINE(dc_net_thread_id, CONFIG_DC_NET_STACK_SIZE, dc_net_thread, NULL, NULL, NULL,
		CONFIG_DC_NET_PRIORITY, 0, 0);
