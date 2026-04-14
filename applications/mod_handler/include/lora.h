/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * LoRa WH-L101-L 驱动接口
 */

#ifndef __LORA_H__
#define __LORA_H__

#include <zephyr/kernel.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief 数据模式下发送透传数据
 *
 * @param data 发送数据
 * @param len  数据长度
 * @return true 发送成功, false 模块繁忙或模式不匹配
 */
bool lora_data_send(const uint8_t *data, size_t len);

/**
 * @brief 进入 AT 指令模式 (+++a 握手)
 *
 * 会停止数据接收, 完成握手后切换到 AT 模式.
 * 调用者后续使用 lora_send_at() 发送指令, 完成后调用 lora_exit_at().
 *
 * @return 0 成功, -EBUSY 模块繁忙, -ETIMEDOUT 握手超时
 */
int lora_enter_at(void);

/**
 * @brief 退出 AT 指令模式 (AT+ENTM), 恢复数据收发
 *
 * @return 0 成功
 */
int lora_exit_at(void);

/**
 * @brief 发送 AT 指令并等待响应
 *
 * 必须在 lora_enter_at() 成功后调用.
 *
 * @param cmd        AT 指令 (不含 \r\n, 函数自动添加)
 * @param resp       响应缓冲区 (可为 NULL)
 * @param resp_size  响应缓冲区大小
 * @param timeout_ms 等待响应超时 (ms)
 * @return 0 成功, -EPERM 未进入 AT 模式, -ETIMEDOUT 超时
 */
int lora_send_at(const char *cmd, char *resp, size_t resp_size,
                 uint32_t timeout_ms);

/**
 * @brief 读取 HOSTWAKE 引脚状态 (模块是否繁忙)
 *
 * @return true 模块正在发送/接收, false 模块空闲
 */
bool lora_get_hostwake_status(void);

/**
 * @brief 设置 HOSTWAKE 引脚状态
 *
 * @param send true 拉高通知模块, false 释放恢复输入
 */
void lora_set_hostwake_status(bool send);

#endif /* __LORA_H__ */
