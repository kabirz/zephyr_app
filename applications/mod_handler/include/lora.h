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
 * HOST_WAKE (Pin 24) 是模块输出引脚:
 * - 高电平: 模块正在串口发送或无线收发中
 * - 低电平: 模块空闲
 *
 * @return true 模块繁忙, false 模块空闲
 */
bool lora_get_hostwake_status(void);

/**
 * @brief LG210 网关连接参数
 */
struct lora_gw_config {
	uint8_t spd;   /* 速率等级 1-12, 默认 10 */
	uint8_t ch;    /* 信道 0-127, 默认 72 (470MHz) */
};

/**
 * @brief 配置 L101 与 LG210 网关连接参数
 *
 * 进入 AT 模式, 依次设置 LORAPROT/SPD/CH, 保存并重启模块.
 * 重启完成后自动恢复数据模式.
 *
 * @param cfg 网关参数 (NULL 则使用默认值: spd=10, ch=72)
 * @return 0 成功, -EBUSY 模块繁忙, -ETIMEDOUT 指令超时
 */
int lora_gw_configure(const struct lora_gw_config *cfg);

/**
 * @brief 查询当前网关连接参数
 *
 * 进入 AT 模式, 读取 SPD/CH, 退出 AT 模式.
 *
 * @param cfg 返回当前参数
 * @return 0 成功, 负数失败
 */
int lora_gw_query(struct lora_gw_config *cfg);

#endif /* __LORA_H__ */
