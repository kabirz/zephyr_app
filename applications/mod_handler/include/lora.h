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
#include <common.h>

/**
 * @brief LoRa 遥测数据帧格式 (8 字节, 与 CAN 0x1E3 一致)
 *
 * Offset  Size  Field
 * 0-1     2     coord_x (int16_t BE, 单位 0.1°)
 * 2-3     2     coord_y (int16_t BE, 单位 0.1°)
 * 4       1     btn flags (bit0: btnHandler 反转, 按下=0, 松开=1)
 * 5-7     3     reserved (0xFF)
 */

/**
 * @brief 打包并发送遥测数据帧
 *
 * 将操纵杆角度、按键状态、电量打包为二进制帧通过 LoRa 发送.
 *
 * @param params 全局参数
 * @return true 发送成功, false 模块繁忙或模式不匹配
 */
bool lora_send_telemetry(const gloval_params_t *params);

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
int lora_send_at(const char *cmd, char *resp, size_t resp_size, uint32_t timeout_ms);

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
 * @brief LG210 网关通信模式 (AT+WMODE)
 */
enum lora_gw_mode {
	LORA_GW_MODE_FP,      /* 点对点模式 */
	LORA_GW_MODE_TRANS,   /* 透传模式 (默认) */
	LORA_GW_MODE_NETWORK, /* 组网模式 */
};

/**
 * @brief LoRa 通信协议类型
 */
enum lora_gw_prot {
	LORA_PROT_NODE,  /* 点对点 (默认) */
	LORA_PROT_LG210, /* LG210 网关 */
	LORA_PROT_LG220, /* LG220 网关 */
};

/**
 * @brief LG210 网关连接参数
 */
struct lora_gw_config {
	enum lora_gw_mode mode; /* 通信模式 */
	enum lora_gw_prot prot; /* 通信协议 */
	uint8_t spd;            /* 速率等级 1-12, 默认 10 */
	uint8_t ch;             /* 信道 0-127, 默认 72 (470MHz) */
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

/* ================================================================
 * LoRa 统一帧格式 (收发一致)
 *
 * [NID 4 bytes LE][Length 2 bytes LE][Data Length bytes][CRC16 2 bytes]
 *
 * NID:    uint32_t LE, 节点 ID
 * Length: uint16_t LE, Data 字段长度
 * Data:   变长数据 (遥测帧 8 字节, 心跳 ACK 0 字节等)
 * CRC16:  CRC16-CCITT, 覆盖 NID + Length + Data (CRC 前所有字节)
 *
 * 遥测帧 (手柄→网关): Data = 8 字节 (与 CAN 0x1E3 一致)
 * 心跳 ACK (网关→手柄): Data = 0 字节 (仅 NID + Length(0) + CRC)
 * 网关数据帧 (网关→手柄): Data = 应用层变长数据
 * ================================================================ */
#define LORA_FRAME_NID_SIZE    4
#define LORA_FRAME_LEN_SIZE    2
#define LORA_FRAME_CRC_SIZE    2
#define LORA_FRAME_HEADER_SIZE (LORA_FRAME_NID_SIZE + LORA_FRAME_LEN_SIZE)    /* 6 */
#define LORA_FRAME_OVERHEAD    (LORA_FRAME_HEADER_SIZE + LORA_FRAME_CRC_SIZE) /* 8 */

/**
 * @brief 设置网关 ID 并写入模块
 *
 * 进入 AT 模式, 发送 AT+GWID, 保存并重启模块, 更新本地缓存.
 * 仅组网模式 (NET) 下有效.
 *
 * @param gwid 网关 ID
 * @return 0 成功, 负数失败
 */
int lora_set_gw_id(uint32_t gwid);

/**
 * @brief 设置节点 ID 并写入模块
 *
 * 进入 AT 模式, 发送 AT+NID, 保存并重启模块, 更新本地缓存.
 *
 * @param nid 新的节点 ID
 * @return 0 成功, 负数失败
 */
int lora_set_node_id(uint32_t nid);

/**
 * @brief LoRa 模块运行时初始化 (上电 + 硬件复位 + 启动 DMA)
 *
 * 上电并通过 RESET 引脚硬件复位模块, 等待 2s 启动完成,
 * 然后重启 DMA 双缓冲接收. 用于 CAN/LoRa 切换和系统唤醒.
 *
 * 注意: UART 回调必须在首次调用前已注册 (由 SYS_INIT 完成).
 *       AT 参数读取仅在 SYS_INIT 中执行一次, 此函数不读参数.
 */
void lora_init(void);

/**
 * @brief LoRa 模块去初始化 (停止 DMA + 断电)
 *
 * 先停止 DMA 接收, 再关闭模块电源. 用于 CAN/LoRa 切换和系统休眠,
 * 避免 UART 引脚悬空导致 DMA 状态损坏.
 */
void lora_deinit(void);

#endif /* __LORA_H__ */
