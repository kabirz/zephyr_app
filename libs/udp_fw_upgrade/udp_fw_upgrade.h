/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * UDP 固件升级库 - 自包含实现 (参考 can_fw_upgrade)
 *
 * 库通过 SYS_INIT 自动创建配置端口 UDP socket (INADDR_ANY:9200),
 * 自管 RX 线程, 内部处理固件升级命令 (FW_START/DATA/END);
 * 其他配置命令通过应用注册的回调分发, 应用无需调用任何 init。
 *
 * 固件升级协议 (配置端口, 帧 [cmd 1B][data...]):
 *   FW_START 0x10 [size 4B LE]  → 回 [0x10][1/0]
 *   FW_DATA  0x11 [data ≤511B]  → 回 [0x11][offset 4B LE]
 *   FW_END   0x12 [test 1B][crc 2B LE] → 回 [0x12][1/0]
 */

#ifndef __UDP_FW_UPGRADE_H__
#define __UDP_FW_UPGRADE_H__

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * 应用命令回调 (注入): 库 RX 线程收到非固件升级命令时调用.
 * 在库的 RX 线程上下文执行, 不可长时间阻塞.
 *
 * @param cmd       命令码 (帧首字节, 已去掉)
 * @param data      命令数据 (cmd 之后的内容)
 * @param len       命令数据长度
 * @param user_data 注册时透传的指针
 * @return true 已处理该命令; false 未处理 (库会告警 unhandled)
 *
 * 回复通过 udp_fw_reply() 发送 (库自管 socket + 回复路由).
 */
typedef bool (*udp_fw_app_cmd_cb_t)(uint8_t cmd, const uint8_t *data,
				    size_t len, void *user_data);

/**
 * @brief 注册业务命令回调
 *
 * 库 RX 线程收到非固件升级命令时调用此回调. 建议在主线程初始化阶段调用
 * (库 RX 线程 SYS_INIT 后启动, 初始化时机足够早).
 *
 * @param cb        回调 (NULL 无操作)
 * @param user_data 透传给回调
 */
void udp_fw_set_app_handler(udp_fw_app_cmd_cb_t cb, void *user_data);

/**
 * @brief 通过配置端口回复上位机
 *
 * 库自管回复路由: 同子网单播到发送方, 跨子网定向广播.
 * 应用在命令回调里调用此函数回复业务命令结果.
 *
 * @param cmd  回复命令码 (通常与请求相同)
 * @param data 回复数据 (NULL 表示无数据)
 * @param len  回复数据长度
 */
void udp_fw_reply(uint8_t cmd, const uint8_t *data, uint8_t len);

#endif /* __UDP_FW_UPGRADE_H__ */
