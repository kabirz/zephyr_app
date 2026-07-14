/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * UDP 透传模块 - nRF24/CAN 与上位机之间的双向 UDP 转发
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/posix/unistd.h>
#include <zephyr/posix/arpa/inet.h>
#include <zephyr/sys/byteorder.h>
#include <gateway.h>

LOG_MODULE_REGISTER(gw_udp, LOG_LEVEL_INF);

static int udp_sock = -1;
static struct sockaddr_in remote_addr;

/* ================================================================
 * UDP 发送 (nRF24/CAN 数据 → 上位机)
 * ================================================================ */
void gw_udp_send(const uint8_t *data, size_t len)
{
	if (udp_sock < 0 || len == 0) {
		return;
	}

	ssize_t sent = sendto(udp_sock, data, len, 0, (struct sockaddr *)&remote_addr,
			      sizeof(remote_addr));
	if (sent < 0) {
		LOG_DBG("UDP send failed: %d", errno);
	}
}

/* ================================================================
 * UDP 接收线程 (上位机 → nRF24/CAN)
 * ================================================================ */
static void udp_rx_thread(void)
{
	udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (udp_sock < 0) {
		LOG_ERR("UDP socket create failed: %d", errno);
		return;
	}

	struct sockaddr_in local_addr = {
		.sin_family = AF_INET,
		.sin_port = htons(gw_params.udp_port),
	};

	if (bind(udp_sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
		LOG_ERR("UDP bind failed: %d", errno);
		close(udp_sock);
		udp_sock = -1;
		return;
	}

	LOG_INF("UDP listening on port %d", gw_params.udp_port);

	/* 默认远程地址: 广播 */
	remote_addr.sin_family = AF_INET;
	remote_addr.sin_port = htons(gw_params.udp_port);
	inet_pton(AF_INET, "255.255.255.255", &remote_addr.sin_addr);

	uint8_t buf[256];

	while (1) {
		struct sockaddr_in src_addr;
		socklen_t addr_len = sizeof(src_addr);

		ssize_t received = recvfrom(udp_sock, buf, sizeof(buf), 0,
					    (struct sockaddr *)&src_addr, &addr_len);
		if (received <= 0) {
			continue;
		}

		/* 记住发送方地址, 后续回复用 */
		remote_addr = src_addr;

		LOG_DBG("UDP recv %zd bytes from %s:%d", received, inet_ntoa(src_addr.sin_addr),
			ntohs(src_addr.sin_port));

		/* 透传到 nRF24 (假设前 2 字节是 CAN ID) */
		if (received >= 2) {
			uint16_t can_id = sys_get_be16(buf);
			gw_rf24_send(can_id, buf + 2, received - 2);
		}
	}
}

K_THREAD_DEFINE(thread_udp_rx, CONFIG_GATEWAY_UDP_STACK, udp_rx_thread, NULL, NULL, NULL,
		CONFIG_GATEWAY_UDP_PRIORITY, 0, 0);
