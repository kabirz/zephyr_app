/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * CAN 固件升级库 - 自包含实现
 * 库通过 SYS_INIT 自动初始化 CAN 控制器并自管 RX 线程与固件升级流程;
 * 应用只需注册一个回调处理非固件升级帧, 无需调用任何 init。
 */

#ifndef __CAN_FW_UPGRADE_H__
#define __CAN_FW_UPGRADE_H__

#include <zephyr/drivers/can.h>
#include <stdbool.h>

/**
 * 应用帧回调 (注入): 库 RX 线程收到非固件升级帧时调用。
 * 在库的 RX 线程上下文执行, 不可长时间阻塞。
 * @return true 已处理该帧; false 未处理 (所有 handler 均返回 false 时,
 *              库 RX 线程会告警 "unhandled CAN frame")
 */
typedef bool (*can_fw_app_rx_cb_t)(struct can_frame *frame, void *user_data);

/**
 * @brief 添加业务帧回调 (可多次调用注册多个)
 *
 * 库 RX 线程收到非固件升级帧时, 按注册顺序广播给所有已注册的回调。
 * 建议在初始化阶段 (RX 线程活跃前) 调用, 避免与 RX 线程并发。
 *
 * @param cb        回调 (NULL 无操作)
 * @param user_data 透传给回调
 * @return CAN 设备指针; NULL 表示 handler 数组已满
 */
const struct device *can_fw_set_app_handler(can_fw_app_rx_cb_t cb, void *user_data);

/**
 * @brief 移除已注册的业务帧回调
 * @return 0 成功, -ENOENT 未找到
 */
int can_fw_remove_handler(can_fw_app_rx_cb_t cb);

#endif /* __CAN_FW_UPGRADE_H__ */
