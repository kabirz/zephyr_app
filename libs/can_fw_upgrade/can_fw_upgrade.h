/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * CAN 固件升级库 - 通用实现
 * 库内部管理 CAN 过滤器和帧处理，应用无需关心具体 CAN ID
 */

#ifndef __CAN_FW_UPGRADE_H__
#define __CAN_FW_UPGRADE_H__

#include <zephyr/drivers/can.h>

/* ================================================================
 * CAN 发送回调类型
 * ================================================================ */
typedef int (*can_fw_send_t)(struct can_frame *frame);

/* ================================================================
 * 固件升级上下文
 * ================================================================ */
struct can_fw_ctx {
	can_fw_send_t send_func;     /* CAN 发送函数 */
	const struct device *can_dev; /* CAN 设备 */
	struct can_filter filter;    /* 接收过滤器 */
};

/**
 * @brief 初始化固件升级并注册 CAN 过滤器
 *
 * @param ctx    固件升级上下文
 * @param dev    CAN 设备
 * @param send   CAN 发送函数
 * @return 0 成功, <0 失败
 */
int can_fw_init(struct can_fw_ctx *ctx, const struct device *dev,
		can_fw_send_t send);

/**
 * @brief CAN 帧接收处理 (在 default case 中调用)
 *
 * @param ctx    固件升级上下文
 * @param frame  接收到的 CAN 帧
 * @return true 已处理, false 未处理
 */
bool can_fw_rx_handler(struct can_fw_ctx *ctx, struct can_frame *frame);

/**
 * @brief 查询固件升级帧是否匹配
 *
 * @param frame  CAN 帧
 * @return true 匹配固件升级帧, false 不匹配
 */
bool can_fw_is_upgrade_frame(struct can_frame *frame);

#endif /* __CAN_FW_UPGRADE_H__ */