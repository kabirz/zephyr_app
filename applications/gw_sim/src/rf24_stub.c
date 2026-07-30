/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * nRF24 stub - native_sim 无射频硬件, 接口空实现 + 日志.
 * udp.c 的数据转发 (gw_rf24_send) 与配置命令 (gw_rf24_set_config) 调用此 stub,
 * 保留调用点以验证 UDP→nRF24 转发路径与配置命令流程.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <gateway.h>

LOG_MODULE_REGISTER(gw_rf24, LOG_LEVEL_INF);

void gw_rf24_init(void)
{
	LOG_INF("rf24 stub init (native_sim, no radio)");
}

void gw_rf24_set_config(uint8_t channel, const uint8_t *addr)
{
	LOG_INF("rf24 stub set_config: ch=%u (addr not applied)", channel);
}

bool gw_rf24_send(uint16_t can_id, const uint8_t *data, size_t len)
{
	LOG_INF("rf24 stub send: id=0x%03x len=%u (not transmitted)", can_id, (unsigned)len);
	LOG_HEXDUMP_DBG(data, len, "rf24 stub payload");
	return true;
}
