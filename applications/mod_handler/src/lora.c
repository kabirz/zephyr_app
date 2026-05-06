/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * LoRa UART 驱动 — WH-L101-L (有人物联网)
 * 基于 Zephyr UART async API + DMA
 *
 * 双模式设计:
 *   数据模式 (LORA_MODE_DATA): 透传收发, DMA 双缓冲, 帧格式 [0xAA][0x55][统一帧][\r\n]
 *   AT 模式   (LORA_MODE_AT):  +++a 握手进入, 一问一答, 同步等待响应 (纯文本)
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <mod-gpio.h>
#include <lora.h>
#include <mod-can.h>

#include <display.h>
LOG_MODULE_REGISTER(lora_serial, LOG_LEVEL_INF);

/* ================================================================
 * 硬件资源
 * ================================================================ */
static const struct gpio_dt_spec lora_reset_pin =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), lorareset_gpios);
static const struct gpio_dt_spec lora_hostwake_pin =
	GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), hostwake_gpios);
static const struct device *uart_dev = DEVICE_DT_GET(DT_NODELABEL(usart2));

/* hostwake_mutex 已移除: HOSTWAKE 仅作输入读取, 无需互斥 */

/* ================================================================
 * 模式状态机
 * ================================================================ */
enum lora_mode {
	LORA_MODE_DATA, /* 透传收发 */
	LORA_MODE_AT,   /* AT 配置 */
};

static atomic_t lora_current_mode = ATOMIC_INIT(LORA_MODE_DATA);
static K_MUTEX_DEFINE(lora_mode_mutex);
static K_MUTEX_DEFINE(lora_tx_mutex); /* 保护 lora_data_send 多线程 TX 竞争 */

/* ================================================================
 * DMA 双缓冲
 * ================================================================ */
#define LORA_BUF_SIZE 256

static uint8_t rx_buf_a[LORA_BUF_SIZE];
static uint8_t rx_buf_b[LORA_BUF_SIZE];
static uint8_t *rx_next_buf = rx_buf_b;
static int rssi_fail_count = 0;

/* ================================================================
 * TX 同步 — 静态缓冲区, DMA 发送完成信号量
 * ================================================================ */
static uint8_t tx_buf[LORA_BUF_SIZE];
static K_SEM_DEFINE(tx_done_sem, 0, 1);

/* ================================================================
 * RX 禁用同步
 * ================================================================ */
static K_SEM_DEFINE(rx_disabled_sem, 0, 1);

/* ================================================================
 * 数据模式消息队列
 * ================================================================ */
struct lora_data_msg {
	uint16_t len;
	uint8_t data[LORA_BUF_SIZE];
};

K_MSGQ_DEFINE(lora_data_msgq, sizeof(struct lora_data_msg), 4, 4);

/* ================================================================
 * AT 模式响应缓冲
 * ================================================================ */
static uint8_t at_resp_buf[LORA_BUF_SIZE];
static uint16_t at_resp_len;
static K_SEM_DEFINE(at_resp_sem, 0, 1);

/* ================================================================
 * 超时参数
 *   DATA: 20ms 帧间空闲即认为一帧结束
 *   AT:   100ms 接收超时, 用于累积完整 AT 响应
 * ================================================================ */
#define LORA_DATA_RX_TIMEOUT 20
#define LORA_AT_RX_TIMEOUT   100

/* ================================================================
 * 链路检测 — RSSI 轮询
 * ================================================================ */
static K_SEM_DEFINE(lora_rssi_sem, 0, 1);

#define LORA_RSSI_PERIOD_MS  10000 /* RSSI 轮询周期 */
#define LORA_RSSI_TIMEOUT_MS 1500  /* RSSI 响应等待超时 */
#define LORA_RSSI_FAIL_MAX   3     /* 连续失败判定断连 */

/* ================================================================
 * HOSTWAKE 管理
 *   HOST_WAKE (Pin 24) 是模块输出引脚:
 *   - 高电平: 模块正在串口发送数据 (拉高 5ms) 或无线发送中
 *   - 低电平: 模块空闲
 *   MCU 仅读取该引脚判断模块忙状态, 不应驱动它
 * ================================================================ */
bool lora_get_hostwake_status(void)
{
	return gpio_pin_get_dt(&lora_hostwake_pin) == 1;
}

/* ================================================================
 * UART Async 回调 — 所有 DMA 事件在此处理
 * ================================================================ */
static void lora_uart_cb(const struct device *dev, struct uart_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);

	switch (evt->type) {
	case UART_RX_RDY:
		if (atomic_get(&lora_current_mode) == LORA_MODE_AT) {
			/* AT 模式: 累积响应, 由 at_resp_sem 通知调用者 */
			uint16_t len = evt->data.rx.len;
			uint16_t off = evt->data.rx.offset;

			if (at_resp_len + len < sizeof(at_resp_buf) - 1) {
				memcpy(at_resp_buf + at_resp_len, evt->data.rx.buf + off, len);
				at_resp_len += len;
				at_resp_buf[at_resp_len] = '\0';
			}
			k_sem_give(&at_resp_sem);
		} else {
			/* 数据模式: 投递完整帧到消息队列 */
			struct lora_data_msg msg = {0};
			uint16_t len = evt->data.rx.len;
			uint16_t off = evt->data.rx.offset;

			msg.len = (len > LORA_BUF_SIZE) ? LORA_BUF_SIZE : len;
			memcpy(msg.data, evt->data.rx.buf + off, msg.len);
			k_msgq_put(&lora_data_msgq, &msg, K_NO_WAIT);
		}
		break;

	case UART_RX_BUF_REQUEST:
		/* 双缓冲: 提供下一个缓冲区, 实现无缝切换 */
		uart_rx_buf_rsp(dev, rx_next_buf, LORA_BUF_SIZE);
		rx_next_buf = (rx_next_buf == rx_buf_a) ? rx_buf_b : rx_buf_a;
		break;

	case UART_RX_BUF_RELEASED:
		break;

	case UART_RX_DISABLED:
		k_sem_give(&rx_disabled_sem);
		break;

	case UART_RX_STOPPED:
		LOG_WRN("UART RX stopped");
		break;

	case UART_TX_DONE:
		k_sem_give(&tx_done_sem);
		break;

	case UART_TX_ABORTED:
		k_sem_give(&tx_done_sem);
		break;

	default:
		break;
	}
}

/* ================================================================
 * 内部 TX 辅助 — 拷贝到静态缓冲区后启动 DMA 发送, 等待完成
 * ================================================================ */
static int lora_async_tx(const uint8_t *data, size_t len)
{
	if (len > sizeof(tx_buf)) {
		return -EINVAL;
	}

	memcpy(tx_buf, data, len);
	k_sem_reset(&tx_done_sem);

	int ret = uart_tx(uart_dev, tx_buf, len, 0);
	if (ret != 0) {
		return ret;
	}

	return k_sem_take(&tx_done_sem, K_MSEC(1000));
}

/* ================================================================
 * 停止 RX 并等待完成
 * ================================================================ */
static void lora_rx_disable_sync(void)
{
	k_sem_reset(&rx_disabled_sem);
	uart_rx_disable(uart_dev);
	k_sem_take(&rx_disabled_sem, K_MSEC(200));
}

/* ================================================================
 * 数据模式发送
 * ================================================================ */
bool lora_data_send(const uint8_t *data, size_t len)
{
	uint8_t tx_data[128];
	int offset = 0;

	if (atomic_get(&lora_current_mode) != LORA_MODE_DATA) {
		LOG_WRN("Not in data mode");
		return false;
	}

	if (!lora_get_link_status()) {
		LOG_WRN("Lora not connect");
		return false;
	}

	k_mutex_lock(&lora_tx_mutex, K_FOREVER);

	/* 帧头: 0xAA 0x55 */
	tx_data[offset++] = 0xAA;
	tx_data[offset++] = 0x55;

	/* NID (4 bytes BE) */
	sys_put_be32(global_params.nid, tx_data + offset);
	offset += LORA_FRAME_NID_SIZE;

	/* Length (2 bytes BE) — Data 字段长度 */
	sys_put_be16((uint16_t)len, tx_data + offset);
	offset += LORA_FRAME_LEN_SIZE;

	/* Data */
	memcpy(tx_data + offset, data, len);
	offset += len;

	/* CRC16 — 覆盖 NID + Length + Data (不含帧头帧尾) */
	uint16_t crc = crc16_ccitt(0, tx_data + 2, offset - 2);

	sys_put_be16(crc, tx_data + offset);
	offset += LORA_FRAME_CRC_SIZE;

	/* 帧尾: \r\n */
	tx_data[offset++] = '\r';
	tx_data[offset++] = '\n';

	/* 等待模块空闲 (HOSTWAKE 低 = 模块未在无线收发) */
	int retry = 40;

	while (lora_get_hostwake_status() && retry-- > 0) {
		k_msleep(5);
	}
	if (retry <= 0) {
		LOG_WRN("LoRa module busy");
		k_mutex_unlock(&lora_tx_mutex);
		return false;
	}

	int ret = lora_async_tx(tx_data, offset);

	k_mutex_unlock(&lora_tx_mutex);

	if (ret != 0) {
		LOG_ERR("lora tx error");
		return false;
	}

	return true;
}

/* ================================================================
 * 遥测数据帧打包发送 — Data: [0x01][X 2B BE][Y 2B BE][btn][0xFF 3B]
 * ================================================================ */
bool lora_send_telemetry(const gloval_params_t *params)
{
	uint8_t frame[9];

	frame[0] = LORA_DATA_HANDLER;
	sys_put_be16((uint16_t)params->x_degree, &frame[1]);
	sys_put_be16((uint16_t)params->y_degree, &frame[3]);
	frame[5] = params->h_button ? 0x00 : 0x01;
	frame[6] = 0xFF;
	frame[7] = 0xFF;
	frame[8] = 0xFF;

	return lora_data_send(frame, sizeof(frame));
}

/* ================================================================
 * RSSI 信号强度请求 — Data: [0x03]
 * ================================================================ */
bool lora_send_rssi_request(void)
{
	uint8_t type = LORA_DATA_RSSI;

	return lora_data_send(&type, 1);
}

/* ================================================================
 * AT 模式进入 — +++ → a → +OK 握手
 *
 * 时序要求 (WH-L101-L):
 *   T2 < 300ms  (+++ 字符间隔)
 *   T3 < 300ms  (+++ 到模块返回 a)
 *   T5 < 3s     (收到 a 后发送确认 a)
 * ================================================================ */
int lora_enter_at(void)
{
	int ret;

	k_mutex_lock(&lora_mode_mutex, K_FOREVER);

	/* 等待模块空闲 */
	int retry = 10;
	while (lora_get_hostwake_status() && retry-- > 0) {
		k_msleep(50);
	}
	if (retry <= 0) {
		LOG_ERR("Module busy, cannot enter AT mode");
		k_mutex_unlock(&lora_mode_mutex);
		return -EBUSY;
	}

	/* 停止数据模式 DMA 接收 */
	lora_rx_disable_sync();

	/* 静默期: 确保 +++ 前无串口活动, 模块才能识别为模式切换 */
	k_msleep(200);

	/* 切换到 AT 模式 */
	atomic_set(&lora_current_mode, LORA_MODE_AT);
	at_resp_len = 0;
	k_sem_reset(&at_resp_sem);

	/* 以 AT 超时重启 RX */
	uart_rx_enable(uart_dev, rx_buf_a, LORA_BUF_SIZE, LORA_AT_RX_TIMEOUT);

	/* Step 1: 发送 +++ */
	ret = lora_async_tx((const uint8_t *)"+++", 3);
	if (ret != 0) {
		LOG_ERR("Failed to send +++");
		goto fail;
	}

	/* Step 2: 等待模块返回 'a' */
	if (k_sem_take(&at_resp_sem, K_MSEC(500)) != 0) {
		LOG_ERR("Timeout waiting for 'a'");
		goto fail;
	}

	/* Step 3: 发送确认码 'a' */
	at_resp_len = 0;
	k_sem_reset(&at_resp_sem);
	ret = lora_async_tx((const uint8_t *)"a", 1);
	if (ret != 0) {
		goto fail;
	}

	/* Step 4: 等待 +OK */
	if (k_sem_take(&at_resp_sem, K_SECONDS(3)) != 0) {
		LOG_ERR("Timeout waiting for +OK");
		goto fail;
	}

	LOG_INF("Entered AT mode");
	return 0;

fail:
	LOG_ERR("Enter AT mode failed, restoring data mode");
	lora_rx_disable_sync();
	atomic_set(&lora_current_mode, LORA_MODE_DATA);
	uart_rx_enable(uart_dev, rx_buf_a, LORA_BUF_SIZE, LORA_DATA_RX_TIMEOUT);
	k_mutex_unlock(&lora_mode_mutex);
	return -ETIMEDOUT;
}

/* ================================================================
 * AT 指令收发 — 一问一答, 等待 OK/ERR 判定响应完整
 * ================================================================ */
int lora_send_at(const char *cmd, char *resp, size_t resp_size, uint32_t timeout_ms)
{
	if (atomic_get(&lora_current_mode) != LORA_MODE_AT) {
		return -EPERM;
	}

	/* 清空响应缓冲 */
	at_resp_len = 0;
	at_resp_buf[0] = '\0';
	k_sem_reset(&at_resp_sem);

	/* 发送 AT 指令 (自动追加 \r\n) */
	char tmp[128];
	int len = snprintf(tmp, sizeof(tmp), "%s\r\n", cmd);

	int ret = lora_async_tx((const uint8_t *)tmp, len);
	if (ret != 0) {
		return ret;
	}

	/* 等待完整响应 — 可能触发多次 UART_RX_RDY
	 * AT 响应以 OK 或 ERR 结尾, 检测到即认为完整
	 */
	int64_t deadline = k_uptime_get() + timeout_ms;

	while (k_uptime_get() < deadline) {
		int32_t remaining = (int32_t)(deadline - k_uptime_get());
		if (remaining <= 0) {
			break;
		}

		ret = k_sem_take(&at_resp_sem, K_MSEC(remaining));
		if (ret != 0) {
			break; /* 超时 */
		}

		/* 响应包含 OK 或 ERR, 认为完整 */
		if (strstr((const char *)at_resp_buf, "OK") ||
		    strstr((const char *)at_resp_buf, "ERR")) {
			break;
		}
	}

	if (resp && resp_size > 0) {
		strncpy(resp, (const char *)at_resp_buf, resp_size - 1);
		resp[resp_size - 1] = '\0';
	}

	return (at_resp_len > 0) ? 0 : -ETIMEDOUT;
}

/* ================================================================
 * AT 模式退出 — AT+ENTM
 * ================================================================ */
int lora_exit_at(void)
{
	char resp[128];
	/* 发送退出指令, 等待 +OK */
	lora_send_at("AT+ENTM\r\n", resp, sizeof(resp), 2000);

	/* 停止 AT 模式 RX */
	lora_rx_disable_sync();

	/* 切换回数据模式 */
	atomic_set(&lora_current_mode, LORA_MODE_DATA);
	uart_rx_enable(uart_dev, rx_buf_a, LORA_BUF_SIZE, LORA_DATA_RX_TIMEOUT);

	LOG_INF("Exited AT mode, back to data mode");
	k_mutex_unlock(&lora_mode_mutex);
	return 0;
}

/* ================================================================
 * LG210 网关配置 — LORAPROT + SPD + CH + 保存 + 重启
 * ================================================================ */
int lora_gw_configure(const struct lora_gw_config *cfg)
{
	struct lora_gw_config defaults = {
		.mode = LORA_GW_MODE_TRANS,
		.prot = LORA_PROT_LG210,
		.spd = 10,
		.ch = 72,
	};

	if (!cfg) {
		cfg = &defaults;
	}

	if (cfg->spd < 1 || cfg->spd > 12) {
		LOG_ERR("Invalid SPD %d (1-12)", cfg->spd);
		return -EINVAL;
	}

	if (cfg->ch > 127) {
		LOG_ERR("Invalid CH %d (0-127)", cfg->ch);
		return -EINVAL;
	}

	int ret = lora_enter_at();

	if (ret) {
		return ret;
	}

	char resp[128];
	char cmd[64];

	/* 选择通信协议 */
	const char *prot_str;

	switch (cfg->prot) {
	case LORA_PROT_LG210:
		prot_str = "LG210";
		break;
	case LORA_PROT_LG220:
		prot_str = "LG220";
		break;
	default:
		prot_str = "NODE";
		break;
	}
	snprintf(cmd, sizeof(cmd), "AT+LORAPROT=%s", prot_str);
	ret = lora_send_at(cmd, resp, sizeof(resp), 2000);
	if (ret) {
		LOG_ERR("Set LORAPROT failed");
		goto out;
	}

	/* 设置速率等级 */
	snprintf(cmd, sizeof(cmd), "AT+SPD=%d", cfg->spd);
	ret = lora_send_at(cmd, resp, sizeof(resp), 2000);
	if (ret) {
		LOG_ERR("Set SPD failed");
		goto out;
	}

	/* 设置信道 */
	snprintf(cmd, sizeof(cmd), "AT+CH=%d", cfg->ch);
	ret = lora_send_at(cmd, resp, sizeof(resp), 2000);
	if (ret) {
		LOG_ERR("Set CH failed");
		goto out;
	}

	/* 设置工作模式 — FP=点对点, TRANS=透传, NET=组网 */
	const char *wmode_str;

	switch (cfg->mode) {
	case LORA_GW_MODE_FP:
		wmode_str = "FP";
		break;
	case LORA_GW_MODE_NETWORK:
		wmode_str = "NET";
		break;
	default:
		wmode_str = "TRANS";
		break;
	}

	snprintf(cmd, sizeof(cmd), "AT+WMODE=%s", wmode_str);
	ret = lora_send_at(cmd, resp, sizeof(resp), 2000);
	if (ret) {
		LOG_ERR("Set WMODE failed");
		goto out;
	}

	/* 重启使配置生效 */
	lora_send_at("AT+Z", resp, sizeof(resp), 2000);

	/* 模块重启后直接进入透传模式, 恢复数据模式 RX */
	k_msleep(1000);
	lora_rx_disable_sync();
	atomic_set(&lora_current_mode, LORA_MODE_DATA);
	uart_rx_enable(uart_dev, rx_buf_a, LORA_BUF_SIZE, LORA_DATA_RX_TIMEOUT);
	k_mutex_unlock(&lora_mode_mutex);

	LOG_INF("Gateway configured: prot=%s mode=%s spd=%d ch=%d", prot_str,
		cfg->mode == LORA_GW_MODE_NETWORK ? "network" : "trans", cfg->spd, cfg->ch);
	return 0;

out:
	lora_exit_at();
	return ret;
}

/* ================================================================
 * LG210 网关参数查询 — 读取 SPD/CH
 * ================================================================ */
static int parse_at_number(const char *resp)
{
	/* AT 查询响应格式: \r\n+CMD:<value>\r\n\r\nOK\r\n
	 * 例: \r\n+GWID:5\r\n\r\nOK\r\n
	 *     \r\n+SPD:10\r\n\r\nOK\r\n
	 * 查找 ':' 或 '=' 作为值分隔符
	 */
	const char *p = strchr(resp, ':');

	if (p) {
		return (int)strtol(p + 1, NULL, 10);
	}

	p = strchr(resp, '=');
	if (p) {
		return (int)strtol(p + 1, NULL, 10);
	}

	return 0;
}

/* ================================================================
 * LG210 十六进制参数查询
 * ================================================================ */
static int parse_at_hex(const char *resp)
{
	/* AT 查询响应格式: \r\n+CMD:<value>\r\n\r\nOK\r\n
	 * 例: \r\n+GWID:5\r\n\r\nOK\r\n
	 * 查找 ':' 或 '=' 作为值分隔符
	 */
	const char *p = strchr(resp, ':');

	if (p) {
		return (int)strtol(p + 1, NULL, 16);
	}

	p = strchr(resp, '=');
	if (p) {
		return (int)strtol(p + 1, NULL, 16);
	}

	return 0;
}
int lora_gw_query(struct lora_gw_config *cfg)
{
	if (!cfg) {
		return -EINVAL;
	}

	int ret = lora_enter_at();

	if (ret) {
		return ret;
	}

	char resp[128];

	/* 查询通信协议 */
	ret = lora_send_at("AT+LORAPROT", resp, sizeof(resp), 2000);
	if (ret == 0) {
		/* 响应格式: \r\n+LORAPROT:LG210\r\n\r\nOK\r\n */
		if (strstr(resp, "LG210")) {
			cfg->prot = LORA_PROT_LG210;
		} else if (strstr(resp, "LG220")) {
			cfg->prot = LORA_PROT_LG220;
		} else {
			cfg->prot = LORA_PROT_NODE;
		}
	} else {
		LOG_WRN("Query LORAPROT failed");
	}

	/* 查询工作模式 — AT+WMODE: FP=点对点, TRANS=透传, NET=组网 */
	ret = lora_send_at("AT+WMODE", resp, sizeof(resp), 2000);
	if (ret == 0) {
		/* 响应格式: \r\n+WMODE:TRANS\r\n\r\nOK\r\n */
		if (strstr(resp, "TRANS")) {
			cfg->mode = LORA_GW_MODE_TRANS;
		} else if (strstr(resp, "NET")) {
			cfg->mode = LORA_GW_MODE_NETWORK;
		} else {
			cfg->mode = LORA_GW_MODE_FP;
		}
		/* TRANS 或默认都是透传模式 */
	} else {
		LOG_WRN("Query WMODE failed");
	}

	/* 查询速率 */
	ret = lora_send_at("AT+SPD", resp, sizeof(resp), 2000);
	if (ret == 0) {
		cfg->spd = (uint8_t)parse_at_number(resp);
	} else {
		LOG_WRN("Query SPD failed");
		cfg->spd = 0;
	}

	/* 查询信道 */
	ret = lora_send_at("AT+CH", resp, sizeof(resp), 2000);
	if (ret == 0) {
		cfg->ch = (uint8_t)parse_at_number(resp);
	} else {
		LOG_WRN("Query CH failed");
		cfg->ch = 0;
	}

	lora_exit_at();
	return 0;
}

/* ================================================================
 * 统一帧解析 — 验证 NID + Length + CRC16
 *
 * 输入: 剥离帧头 0xAA 0x55 和帧尾 \r\n 后的统一帧数据
 * 输出: payload 指向 Data 字段, payload_len 为 Data 长度
 * 返回: true 帧合法 (NID 匹配 + CRC 正确)
 * ================================================================ */
static bool parse_lora_frame(const uint8_t *data, uint16_t len, const uint8_t **payload,
			     uint16_t *payload_len)
{
	/* 最小帧: NID(4) + Length(2) + CRC(2) = 8 */
	if (len < LORA_FRAME_OVERHEAD) {
		LOG_ERR("size(%d) < 8", len);
		return false;
	}

	/* 验证 NID */
	uint32_t rx_nid = sys_get_be32(data);
	if (rx_nid != global_params.nid) {
		LOG_ERR("NID is not right: %x", rx_nid);
		return false;
	}

	/* 提取 Length (Data 字段长度) */
	uint16_t data_len = sys_get_be16(data + LORA_FRAME_NID_SIZE);

	/* 检查帧完整性 */
	if (len != LORA_FRAME_HEADER_SIZE + data_len + LORA_FRAME_CRC_SIZE) {
		LOG_ERR("frame is not right");
		return false;
	}

	/* 验证 CRC16 — 覆盖 NID + Length + Data */
	uint16_t calc_crc = crc16_ccitt(0, data, LORA_FRAME_HEADER_SIZE + data_len);
	uint16_t rx_crc = sys_get_be16(data + LORA_FRAME_HEADER_SIZE + data_len);

	if (calc_crc != rx_crc) {
		LOG_ERR("crc is not right");
		return false;
	}

	if (payload) {
		*payload = data + LORA_FRAME_HEADER_SIZE;
	}
	if (payload_len) {
		*payload_len = data_len;
	} else {
		LOG_ERR("data len is zero");
		return false;
	}
	return true;
}

/* ================================================================
 * 数据接收处理线程 — 帧边界检测 + 统一帧解析 + 类型分发
 *
 * DMA 可能将一个完整帧拆分到多个缓冲区, 也可能多个帧合并到一个缓冲区.
 * 本线程使用 asm_buf 累积数据, 通过帧头 0xAA 0x55 + 帧尾 \r\n
 * 检测完整帧边界, 剥离帧头帧尾后交给 parse_lora_frame 校验, 再按类型分发.
 *
 * 特殊帧: "LoRa Start!" — 模块启动消息, 无 0xAA 0x55 帧头
 * ================================================================ */
#define ASM_BUF_SIZE (LORA_BUF_SIZE * 2)

static void lora_msg_process_thread(void)
{
	static uint8_t asm_buf[ASM_BUF_SIZE];
	static uint16_t asm_len;
	struct lora_data_msg msg;

	while (true) {
		if (k_msgq_get(&lora_data_msgq, &msg, K_FOREVER) != 0) {
			continue;
		}

		/* 追加到重组缓冲区, 溢出时丢弃旧数据重新开始 */
		if (asm_len + msg.len > ASM_BUF_SIZE) {
			LOG_WRN("asm_buf overflow, dropping %d bytes", asm_len);
			if (msg.len > ASM_BUF_SIZE) {
				continue;
			}
			memcpy(asm_buf, msg.data, msg.len);
			asm_len = msg.len;
		} else {
			memcpy(asm_buf + asm_len, msg.data, msg.len);
			asm_len += msg.len;
		}

		/* 循环提取完整帧 */
		while (asm_len > 0) {
			/* 特殊处理: "LoRa Start!" 启动消息 (无帧头) */
			if (asm_len >= 11 &&
			    memcmp(asm_buf, "LoRa Start!", 11) == 0) {
				LOG_INF("Lora start!");
				int skip = 11;
				/* 跳过可能的 \r\n 后缀 */
				if (asm_len >= 13 &&
				    asm_buf[11] == '\r' && asm_buf[12] == '\n') {
					skip = 13;
				}
				asm_len -= skip;
				if (asm_len > 0) {
					memmove(asm_buf, asm_buf + skip, asm_len);
				}
				continue;
			}

			/* 查找帧头 0xAA 0x55 */
			if (asm_buf[0] != 0xAA) {
				/* 非帧头字节, 丢弃 */
				asm_len--;
				if (asm_len > 0) {
					memmove(asm_buf, asm_buf + 1, asm_len);
				}
				continue;
			}

			/* 仅有 0xAA, 等待下一字节确认 */
			if (asm_len < 2) {
				break;
			}

			/* 0xAA 后不是 0x55, 丢弃 0xAA 重新查找 */
			if (asm_buf[1] != 0x55) {
				asm_len--;
				if (asm_len > 0) {
					memmove(asm_buf, asm_buf + 1, asm_len);
				}
				continue;
			}

			/* 找到帧头, 在后续数据中搜索 \r\n 帧尾 */
			int tail_pos = -1;

			for (int i = 2; i + 1 < asm_len; i++) {
				if (asm_buf[i] == 0x0D && asm_buf[i + 1] == 0x0A) {
					tail_pos = i;
					break;
				}
			}

			if (tail_pos < 0) {
				/* 未找到帧尾, 等待更多数据 */
				break;
			}

			/* 完整帧: [0xAA][0x55][content][\r\n] */
			uint16_t content_len = tail_pos - 2;
			uint16_t total_len = tail_pos + 2;

			if (content_len >= LORA_FRAME_OVERHEAD) {
				const uint8_t *payload;
				uint16_t payload_len;

				if (parse_lora_frame(asm_buf + 2, content_len,
						     &payload, &payload_len)) {
					uint8_t type = payload[0];

					switch (type) {
					case LORA_DATA_RSSI:
						if (payload_len >= 2) {
							uint8_t rssi = payload[1];

							/* if (rssi > 4) { */
								/* rssi = 4; */
							/* } */
							if (global_params.rssi != rssi) {
								global_params.rssi = rssi;
								mod_display_lora(rssi);
								LOG_INF("LoRa RSSI level: %d", rssi);
							}
							rssi_fail_count = 0;
							LOG_INF("RSSI ACK!");
							k_sem_give(&lora_rssi_sem);
						}
						break;

					case LORA_DATA_HANDLER: {
						/* 网关下发的扫描仪数据:
						 * [type 1B][CAN frame ID 2B BE][CAN data NB]
						 */
						if (payload_len >= 3) {
							uint16_t frame_id = sys_get_be16(payload + 1);
							uint16_t data_len2 = payload_len - 3;

							if (data_len2 <=
							    sizeof(((struct can_frame *)0)->data)) {
								struct can_frame frame = {
									.id = frame_id,
									.dlc = can_bytes_to_dlc(data_len2),
								};
								memcpy(frame.data, payload + 3,
								       data_len2);
								mod_can_parse_scanner(&frame);
							} else {
								LOG_WRN("LoRa RX data too long: %d",
									data_len2);
							}
						}
						break;
					}
					case LORA_DATA_TEST:
						LOG_HEXDUMP_INF(asm_buf + 2, content_len,
								"LoRa RX (Test):");
						break;

					default:
						LOG_WRN("Unknown LoRa data type: 0x%02x", type);
						break;
					}
				} else {
					LOG_WRN("Frame parse failed (content_len=%d)", content_len);
				}
			} else if (content_len > 0) {
				LOG_WRN("Frame content too short: %d", content_len);
			}

			/* 前进到下一帧 */
			asm_len -= total_len;
			if (asm_len > 0) {
				memmove(asm_buf, asm_buf + total_len, asm_len);
			}
		}
	}
}
K_THREAD_DEFINE(lora_msg, 1024, lora_msg_process_thread, NULL, NULL, NULL, 12, 0, 0);

/* ================================================================
 * LoRa RSSI 轮询线程 — 仅 connect_type == LORA_TYPE 时运行
 *
 * 周期发送 RSSI 请求帧, 等待网关响应.
 * 连续 LORA_RSSI_FAIL_MAX 次失败判定链路断开.
 * ================================================================ */
static void lora_rssi_thread(void)
{
	while (true) {
		if (global_params.sleeping) {
			k_event_wait(&global_params.event, WAKE_EVENT, false, K_FOREVER);
			continue;
		}
		k_event_wait(&global_params.event, LORA_EVENT, false, K_FOREVER);

		/* AT 模式: 跳过 RSSI 轮询 */
		if (atomic_get(&lora_current_mode) == LORA_MODE_AT) {
			k_sleep(K_MSEC(LORA_RSSI_PERIOD_MS));
			continue;
		}

		uint32_t t1 = k_uptime_get_32();
		k_sem_reset(&lora_rssi_sem);

		bool sent = lora_send_rssi_request();

		if (sent) {
			if (k_sem_take(&lora_rssi_sem, K_MSEC(LORA_RSSI_TIMEOUT_MS)) == 0) {
				if (rssi_fail_count > 0) {
					LOG_INF("LoRa link restored");
				}
				rssi_fail_count = 0;
			} else {
				rssi_fail_count++;
				LOG_WRN("LoRa RSSI timeout (%d/%d)", rssi_fail_count,
					LORA_RSSI_FAIL_MAX);
			}
		} else {
			rssi_fail_count++;
			LOG_WRN("LoRa RSSI send failed (%d/%d)", rssi_fail_count,
				LORA_RSSI_FAIL_MAX);
		}

		if (rssi_fail_count >= LORA_RSSI_FAIL_MAX) {
			global_params.rssi = 0;
			mod_display_lora(0);
			rssi_fail_count = 0;
		}

		uint32_t diff = k_uptime_get_32() - t1;
		if (LORA_RSSI_PERIOD_MS > diff)
			k_sleep(K_MSEC(LORA_RSSI_PERIOD_MS - diff));
	}
}
K_THREAD_DEFINE(lora_heart, 1024, lora_rssi_thread, NULL, NULL, NULL, 12, 0, 0);
/* ================================================================
 * 测试发送线程 — 每 200ms 发送一帧, 携带递增 index (uint16_t BE)
 * Data: [LORA_DATA_TEST][index_hi][index_lo]
 * ================================================================ */
static void lora_test_tx_thread(void)
{
	static uint16_t test_index = 0;
	uint8_t frame[3];

	frame[0] = LORA_DATA_TEST;

	while (true) {
		if (global_params.sleeping) {
			k_event_wait(&global_params.event, WAKE_EVENT, false, K_FOREVER);
			continue;
		}

		test_index++;
		sys_put_be16(test_index, &frame[1]);

		bool ok = lora_data_send(frame, sizeof(frame));
		/* LOG_INF("test tx: index=%u %s", test_index, ok ? "OK" : "FAIL"); */
		if (!ok)
			test_index--;

		k_msleep(200);
	}
}
K_THREAD_DEFINE(lora_test_tx, 512, lora_test_tx_thread, NULL, NULL, NULL, 12, 0, 0);

/* ================================================================
 * 网关 ID 设置 — 独立于通信参数
 * ================================================================ */
int lora_set_gw_id(uint32_t gwid)
{
	int ret = lora_enter_at();

	if (ret) {
		return ret;
	}

	char resp[128];
	char cmd[64];

	snprintf(cmd, sizeof(cmd), "AT+GWID=%08X", gwid);
	ret = lora_send_at(cmd, resp, sizeof(resp), 2000);
	if (ret) {
		LOG_ERR("Set GWID failed");
		lora_exit_at();
		return ret;
	}

	lora_send_at("AT+Z", resp, sizeof(resp), 2000);

	/* 模块重启后直接进入透传模式, 更新本地缓存 */
	global_params.gwid = gwid;
	k_msleep(50);
	lora_rx_disable_sync();
	atomic_set(&lora_current_mode, LORA_MODE_DATA);
	uart_rx_enable(uart_dev, rx_buf_a, LORA_BUF_SIZE, LORA_DATA_RX_TIMEOUT);
	k_mutex_unlock(&lora_mode_mutex);

	LOG_INF("GWID set to %08X", gwid);
	return 0;
}

int lora_set_node_id(uint32_t nid)
{
	int ret = lora_enter_at();

	if (ret) {
		return ret;
	}

	char resp[128];
	char cmd[64];

	snprintf(cmd, sizeof(cmd), "AT+NID=%08X", nid);
	ret = lora_send_at(cmd, resp, sizeof(resp), 2000);
	if (ret) {
		LOG_ERR("Set NID failed");
		lora_exit_at();
		return ret;
	}

	lora_send_at("AT+Z", resp, sizeof(resp), 2000);

	/* 模块重启后直接进入透传模式, 更新本地缓存 */
	global_params.nid = nid;
	mod_display_lora_nid(global_params.nid);
	k_msleep(50);
	lora_rx_disable_sync();
	atomic_set(&lora_current_mode, LORA_MODE_DATA);
	uart_rx_enable(uart_dev, rx_buf_a, LORA_BUF_SIZE, LORA_DATA_RX_TIMEOUT);
	k_mutex_unlock(&lora_mode_mutex);

	LOG_INF("Node ID set to %08X", nid);
	return 0;
}

/* ================================================================
 * Shell 调试命令
 *   lora send <data>   — 透传模式发送
 *   lora at <cmd>      — AT 指令 (自动进入 AT 模式)
 *   lora exit          — 退出 AT 模式
 * ================================================================ */
#ifdef CONFIG_SHELL
#include <zephyr/shell/shell.h>

static int cmd_send(const struct shell *ctx, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(ctx, "Usage: lora send <data>");
		return -EINVAL;
	}
	uint8_t tx_data[64];
	tx_data[0] = LORA_DATA_TEST;
	size_t len = strlen(argv[1]);
	memcpy(tx_data+1, argv[1], len);

	if (!lora_data_send(tx_data, len+1)) {
		shell_error(ctx, "LoRa busy or not in data mode");
		return -EIO;
	}
	return 0;
}

static int cmd_at(const struct shell *ctx, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(ctx, "Usage: lora at <AT command>");
		return -EINVAL;
	}

	/* 自动进入 AT 模式 */
	if (atomic_get(&lora_current_mode) != LORA_MODE_AT) {
		int ret = lora_enter_at();

		if (ret != 0) {
			shell_error(ctx, "Failed to enter AT mode (%d)", ret);
			return ret;
		}
	}

	char resp[256];
	char at_cmd[256] = {0};

	if (strncmp(argv[1], "AT+", 3)) {
		snprintf(at_cmd, sizeof(at_cmd), "AT+%s\r\n", argv[1]);
	} else {
		snprintf(at_cmd, sizeof(at_cmd), "%s\r\n", argv[1]);
	}
	int ret = lora_send_at(at_cmd, resp, sizeof(resp), 2000);

	if (ret == 0) {
		shell_print(ctx, "%s", resp);
	} else {
		shell_error(ctx, "AT command timeout");
	}
	if (strncmp("AT+Z", at_cmd, 4) == 0) {
		shell_print(ctx, "reboot WH-L101-L");
		/* 停止 AT 模式 RX */
		lora_rx_disable_sync();

		/* 切换回数据模式 */
		atomic_set(&lora_current_mode, LORA_MODE_DATA);
		uart_rx_enable(uart_dev, rx_buf_a, LORA_BUF_SIZE, LORA_DATA_RX_TIMEOUT);
		return 0;
	} else {
		return lora_exit_at();
	}
}

static int cmd_gw_config(const struct shell *ctx, size_t argc, char **argv)
{
	struct lora_gw_config cfg = {
		.mode = LORA_GW_MODE_TRANS,
		.prot = LORA_PROT_LG210,
		.spd = 10,
		.ch = 72,
	};

	/* lora gw config [prot] [mode] [spd] [ch]
	 * prot: "node" (default), "lg210", "lg220"
	 * mode: "trans" (default), "fp", "net"
	 */
	if (argc >= 2) {
		if (strcmp(argv[1], "lg210") == 0) {
			cfg.prot = LORA_PROT_LG210;
		} else if (strcmp(argv[1], "lg220") == 0) {
			cfg.prot = LORA_PROT_LG220;
		}
	}
	if (argc >= 3) {
		if (strcmp(argv[2], "net") == 0) {
			cfg.mode = LORA_GW_MODE_NETWORK;
		} else if (strcmp(argv[2], "fp") == 0) {
			cfg.mode = LORA_GW_MODE_FP;
		}
	}
	if (argc >= 4) {
		cfg.spd = (uint8_t)strtol(argv[3], NULL, 10);
	}
	if (argc >= 5) {
		cfg.ch = (uint8_t)strtol(argv[4], NULL, 10);
	}

	int ret = lora_gw_configure(&cfg);

	if (ret) {
		shell_error(ctx, "Gateway config failed (%d)", ret);
		return ret;
	}
	const char *prot_str = cfg.prot == LORA_PROT_LG210   ? "lg210"
			       : cfg.prot == LORA_PROT_LG220 ? "lg220"
							     : "node";
	shell_print(ctx, "Gateway configured: prot=%s mode=%s spd=%d ch=%d", prot_str,
		    cfg.mode == LORA_GW_MODE_NETWORK ? "net"
		    : cfg.mode == LORA_GW_MODE_FP    ? "fp"
						     : "trans",
		    cfg.spd, cfg.ch);
	return 0;
}

static int cmd_gw_query(const struct shell *ctx, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct lora_gw_config cfg;
	int ret = lora_gw_query(&cfg);

	if (ret) {
		shell_error(ctx, "Query failed (%d)", ret);
		return ret;
	}
	const char *prot_str = cfg.prot == LORA_PROT_LG210   ? "lg210"
			       : cfg.prot == LORA_PROT_LG220 ? "lg220"
							     : "node";
	shell_print(ctx, "Gateway params: prot=%s mode=%s spd=%d ch=%d", prot_str,
		    cfg.mode == LORA_GW_MODE_NETWORK ? "net"
		    : cfg.mode == LORA_GW_MODE_FP    ? "fp"
						     : "trans",
		    cfg.spd, cfg.ch);
	return 0;
}

static int cmd_gwid(const struct shell *ctx, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(ctx, "GWID: %08X", global_params.gwid);
		return 0;
	}
	uint32_t gwid = (uint32_t)strtol(argv[1], NULL, 16);
	int ret = lora_set_gw_id(gwid);
	if (ret) {
		shell_error(ctx, "Set GWID failed (%d)", ret);
		return ret;
	}
	shell_print(ctx, "GWID set to %08X", gwid);
	return 0;
}

static int cmd_nid(const struct shell *ctx, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(ctx, "NID: %08X", global_params.nid);
		return 0;
	}
	uint32_t nid = (uint32_t)strtol(argv[1], NULL, 16);
	int ret = lora_set_node_id(nid);
	if (ret) {
		shell_error(ctx, "Set NID failed (%d)", ret);
		return ret;
	}
	shell_print(ctx, "NID set to %08X", nid);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_lora_gw_cmds,
			       SHELL_CMD_ARG(config, NULL,
					     "Configure gateway params and reboot\n"
					     "Usage: config [prot] [mode] [spd] [ch]\n"
					     "  prot: node (default), lg210, lg220\n"
					     "  mode: trans (default), fp, net",
					     cmd_gw_config, 1, 4),
			       SHELL_CMD_ARG(query, NULL, "Query current gateway params",
					     cmd_gw_query, 1, 0),
			       SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(sub_lora_cmds,
			       SHELL_CMD_ARG(send, NULL,
					     "Send data in transparent mode\n"
					     "Usage: send <data>",
					     cmd_send, 2, 0),
			       SHELL_CMD_ARG(at, NULL,
					     "Send AT command (auto enter AT mode)\n"
					     "Usage: at <command>",
					     cmd_at, 2, 0),
			       SHELL_CMD(gw, &sub_lora_gw_cmds, "LG210 gateway operations", NULL),
			       SHELL_CMD_ARG(nid, NULL,
					     "Get/set node ID\n"
					     "Usage: nid [hex_value]",
					     cmd_nid, 1, 1),
			       SHELL_CMD_ARG(gwid, NULL,
					     "Get/set gateway ID\n"
					     "Usage: gwid [hex_value]",
					     cmd_gwid, 1, 1),
			       SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(lora, &sub_lora_cmds, "LoRa WH-L101-L commands", NULL);
#endif /* CONFIG_SHELL */

void lora_init(void)
{
	/* 上电 + 硬件复位 */
	lora_power_enable(true);
	gpio_pin_set_dt(&lora_reset_pin, 0);
	k_msleep(10);
	gpio_pin_set_dt(&lora_reset_pin, 1);
	k_msleep(10);

	/* 等待模块就绪 (HOSTWAKE 低 = 空闲) */
	int retry = 100;
	while (lora_get_hostwake_status() && retry-- > 0) {
		k_msleep(20);
	}
	if (retry <= 0) {
		LOG_WRN("LoRa module boot timeout");
	}

	lora_rx_disable_sync();
	rx_next_buf = rx_buf_b;
	atomic_set(&lora_current_mode, LORA_MODE_DATA);
	uart_rx_enable(uart_dev, rx_buf_a, LORA_BUF_SIZE, LORA_DATA_RX_TIMEOUT);
}

void lora_deinit(void)
{
	lora_rx_disable_sync();
	lora_power_enable(false);
}
/* ================================================================
 * 初始化 — 上电复位 + 注册 async 回调 + 启动 DMA 接收
 * ================================================================ */
static int lora_serial_init(void)
{
	if (!device_is_ready(uart_dev)) {
		LOG_ERR("UART device not ready");
		return -ENODEV;
	}

	/* RESET 引脚 */
	int ret = gpio_pin_configure_dt(&lora_reset_pin, GPIO_OUTPUT);

	if (ret < 0) {
		LOG_ERR("Failed to configure reset pin: %d", ret);
		return ret;
	}

	/* HOSTWAKE 初始化为输入 */
	gpio_pin_configure_dt(&lora_hostwake_pin, GPIO_INPUT | GPIO_PULL_UP);

	/* 注册 async 回调 (必须在 lora_init 之前) */
	uart_callback_set(uart_dev, lora_uart_cb, NULL);

	/* 上电 + 复位 + 等待就绪 + 启动 DMA */
	lora_init();

	/* 进入 AT 模式读取模块参数 (NID/GWID/SPD/CH) */
	struct lora_gw_config cfg;

	ret = lora_gw_query(&cfg);
	if (ret == 0) {
		LOG_INF("LoRa params: prot=%s mode=%s spd=%d ch=%d",
			cfg.prot == LORA_PROT_LG210   ? "LG210"
			: cfg.prot == LORA_PROT_LG220 ? "LG220"
						      : "NODE",
			cfg.mode == LORA_GW_MODE_NETWORK ? "net" : "trans", cfg.spd, cfg.ch);
	} else {
		LOG_WRN("LoRa param query failed (%d), using defaults", ret);
	}

	/* 读取 NID 和 GWID */
	ret = lora_enter_at();
	if (ret == 0) {
		char resp[128];

		ret = lora_send_at("AT+NID", resp, sizeof(resp), 2000);
		if (ret == 0) {
			global_params.nid = (uint32_t)parse_at_hex(resp);
			mod_display_lora_nid(global_params.nid);
			LOG_INF("LoRa NID: 0x%08X", global_params.nid);
		}

		ret = lora_send_at("AT+GWID", resp, sizeof(resp), 2000);
		if (ret == 0) {
			global_params.gwid = (uint32_t)parse_at_hex(resp);
			LOG_INF("LoRa GWID: 0x%08X", global_params.gwid);
		}

		lora_exit_at();
	}

	LOG_INF("LoRa WH-L101-L initialized (async DMA, buf=%d, timeout=%dms)", LORA_BUF_SIZE,
		LORA_DATA_RX_TIMEOUT);
	return 0;
}

SYS_INIT(lora_serial_init, APPLICATION, 10);
