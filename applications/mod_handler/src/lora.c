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
#include <zephyr/sys/ring_buffer.h>
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
static const struct gpio_dt_spec lora_reset_pin = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), lorareset_gpios);
static const struct gpio_dt_spec lora_hostwake_pin = GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), hostwake_gpios);
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
static K_MUTEX_DEFINE(lora_data_mutex);
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
 * LORA 等待数据回复
 * ================================================================ */
static K_SEM_DEFINE(lora_data_sem, 0, 1);
#define LORA_TELEM_TIMEOUT_MS 500

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
static K_SEM_DEFINE(lora_test_sem, 0, 1);

#define LORA_RSSI_PERIOD_MS  4000 /* RSSI 轮询周期 */
#define LORA_RSSI_TIMEOUT_MS 1500 /* RSSI 响应等待超时 */
#define LORA_RSSI_FAIL_MAX   3    /* 连续失败判定断连 */

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
		global_params.rssi = 0;
		mod_display_lora(0);
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
bool lora_send_telemetry(const global_params_t *params)
{
	if (global_params.test_mode) {
		return false;
	}

	uint8_t frame[9];

	frame[0] = LORA_DATA_HANDLER;
	sys_put_be16((uint16_t)params->x_degree, &frame[1]);
	sys_put_be16((uint16_t)params->y_degree, &frame[3]);
	frame[5] = params->h_button ? 0x00 : 0x01;
	frame[6] = 0xFF;
	frame[7] = 0xFF;
	frame[8] = 0xFF;

	k_sem_reset(&lora_data_sem);
	k_mutex_lock(&lora_data_mutex, K_FOREVER);
	bool ok = lora_data_send(frame, sizeof(frame));
	if (ok) {
		/* 分段等待, 每 100ms 检查 sleep, 避免休眠时长时间持锁 */
		for (int i = 0; i < LORA_TELEM_TIMEOUT_MS / 100; i++) {
			if (k_sem_take(&lora_data_sem, K_MSEC(100)) == 0) {
				break;
			}
			if (global_params.sleeping) {
				break;
			}
		}
	}
	k_mutex_unlock(&lora_data_mutex);
	return ok;
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
	k_msleep(10);

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
 * AT+Z 重启模块并恢复数据模式
 * ================================================================ */
static int lora_restart_to_data_mode(uint32_t restart_ms)
{
	char resp[128];

	lora_send_at("AT+Z", resp, sizeof(resp), 2000);
	k_msleep(restart_ms);
	lora_rx_disable_sync();
	atomic_set(&lora_current_mode, LORA_MODE_DATA);
	uart_rx_enable(uart_dev, rx_buf_a, LORA_BUF_SIZE, LORA_DATA_RX_TIMEOUT);
	k_mutex_unlock(&lora_mode_mutex);
	return 0;
}

/* ================================================================
 * AT 参数设置统一实现
 * ================================================================ */
static int lora_set_at_param(const char *cmd, const char *tag)
{
	int ret = lora_enter_at();

	if (ret) {
		return ret;
	}

	char resp[128];

	ret = lora_send_at(cmd, resp, sizeof(resp), 2000);
	if (ret) {
		LOG_ERR("Set %s failed", tag);
		lora_exit_at();
		return ret;
	}

	return lora_restart_to_data_mode(50);
}

/* ================================================================
 * AT 模式下发送设置指令 (在已进入 AT 模式后使用)
 * ================================================================ */
static int lora_send_at_set(const char *cmd, const char *tag)
{
	char resp[128];
	int ret = lora_send_at(cmd, resp, sizeof(resp), 2000);

	if (ret) {
		LOG_ERR("Set %s failed", tag);
	}
	return ret;
}

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

/* ================================================================
 * LG210 网关配置 — LORAPROT + SPD + CH + 保存 + 重启
 * ================================================================ */
int lora_configure(const struct lora_config *cfg)
{
	struct lora_config defaults = {
		.mode = LORA_GW_MODE_TRANS,
		.prot = LORA_PROT_LG210,
	};

	if (!cfg) {
		cfg = &defaults;
	}

	int ret = lora_enter_at();
	if (ret) {
		return ret;
	}

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
	ret = lora_send_at_set(cmd, "LORAPROT");
	if (ret) {
		goto out;
	}

	/* 设置工作模式 */
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
	ret = lora_send_at_set(cmd, "WMODE");
	if (ret) {
		goto out;
	}

	/* 设置通道1参数 */
	snprintf(cmd, sizeof(cmd), "AT+SPD1=%d", cfg->spd1);
	ret = lora_send_at_set(cmd, "SPD1");
	if (ret) {
		goto out;
	}

	snprintf(cmd, sizeof(cmd), "AT+CH1=%d", cfg->ch1);
	ret = lora_send_at_set(cmd, "CH1");
	if (ret) {
		goto out;
	}

	/* 设置通道2参数 */
	snprintf(cmd, sizeof(cmd), "AT+SPD2=%d", cfg->spd2);
	ret = lora_send_at_set(cmd, "SPD2");
	if (ret) {
		goto out;
	}

	snprintf(cmd, sizeof(cmd), "AT+CH2=%d", cfg->ch2);
	ret = lora_send_at_set(cmd, "CH2");
	if (ret) {
		goto out;
	}

	/* 设置通道选择 */
	snprintf(cmd, sizeof(cmd), "AT+PNUM=%d", cfg->pnum);
	ret = lora_send_at_set(cmd, "PNUM");
	if (ret) {
		goto out;
	}

	/* 更新本地缓存 */
	global_params.prot = (uint8_t)cfg->prot;
	global_params.mode = (uint8_t)cfg->mode;
	global_params.spd1 = cfg->spd1;
	global_params.ch1 = cfg->ch1;
	global_params.spd2 = cfg->spd2;
	global_params.ch2 = cfg->ch2;
	global_params.pnum = cfg->pnum;

	LOG_INF("Configured: prot=%s mode=%s spd1=%d ch1=%d spd2=%d ch2=%d pnum=%d",
		prot_str, wmode_str, cfg->spd1, cfg->ch1, cfg->spd2, cfg->ch2, cfg->pnum);

	/* 重启使配置生效 */
	return lora_restart_to_data_mode(1000);

out:
	lora_exit_at();
	return ret;
}

int lora_query(struct lora_config *cfg)
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

	/* 查询工作模式 */
	ret = lora_send_at("AT+WMODE", resp, sizeof(resp), 2000);
	if (ret == 0) {
		if (strstr(resp, "TRANS")) {
			cfg->mode = LORA_GW_MODE_TRANS;
		} else if (strstr(resp, "NET")) {
			cfg->mode = LORA_GW_MODE_NETWORK;
		} else {
			cfg->mode = LORA_GW_MODE_FP;
		}
	} else {
		LOG_WRN("Query WMODE failed");
	}

	/* 查询通道1参数 */
	ret = lora_send_at("AT+SPD1", resp, sizeof(resp), 2000);
	if (ret == 0) {
		cfg->spd1 = (uint8_t)parse_at_number(resp);
	} else {
		LOG_WRN("Query SPD1 failed");
		cfg->spd1 = 0;
	}

	ret = lora_send_at("AT+CH1", resp, sizeof(resp), 2000);
	if (ret == 0) {
		cfg->ch1 = (uint16_t)parse_at_number(resp);
	} else {
		LOG_WRN("Query CH1 failed");
		cfg->ch1 = 0;
	}

	/* 查询通道2参数 */
	ret = lora_send_at("AT+SPD2", resp, sizeof(resp), 2000);
	if (ret == 0) {
		cfg->spd2 = (uint8_t)parse_at_number(resp);
	} else {
		LOG_WRN("Query SPD2 failed");
		cfg->spd2 = 0;
	}

	ret = lora_send_at("AT+CH2", resp, sizeof(resp), 2000);
	if (ret == 0) {
		cfg->ch2 = (uint16_t)parse_at_number(resp);
	} else {
		LOG_WRN("Query CH2 failed");
		cfg->ch2 = 0;
	}

	/* 查询通道选择 */
	ret = lora_send_at("AT+PNUM", resp, sizeof(resp), 2000);
	if (ret == 0) {
		cfg->pnum = (uint8_t)parse_at_number(resp);
	} else {
		LOG_WRN("Query PNUM failed");
		cfg->pnum = 0;
	}

	lora_exit_at();
	return 0;
}



/* ================================================================
 * 数值参数解析
 * ================================================================ */



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

/* ================================================================
 * 环形缓冲区: 用于接收数据重组
 * ================================================================ */
static uint8_t lora_rx_buf_data[ASM_BUF_SIZE];
static struct ring_buf lora_rx_rb;

/* ================================================================
 * LoRa 测试统计 — 时延 (RTT) + 丢包率
 *
 * TX: 每 200ms 发送 [0x02][index 2B BE][timestamp 4B BE]
 * RX: 网关回传同一帧, 计算 RTT = now - timestamp, 检测 index 间隔丢包
 * 统计数据存储在 global_params 的 test_* 字段中.
 * ================================================================ */

// 定义信号等级枚举
typedef enum {
	SIGNAL_NONE = 0,     // 无信号/极差
	SIGNAL_BAD = 1,      // 差
	SIGNAL_FAIR = 2,     // 一般
	SIGNAL_GOOD = 3,     // 良好
	SIGNAL_EXCELLENT = 4 // 极好
} SignalQuality_t;

SignalQuality_t get_lora_signal_level(int32_t rssi, int32_t snr)
{
	uint8_t score = 0;

	// --- 1. SNR 评分 (权重较高，因为 LoRa 对信噪比敏感) ---
	if (snr > 10) {
		score += 3; // SNR 极佳
	} else if (snr > 0) {
		score += 2; // SNR 良好
	} else if (snr > -10) {
		score += 1; // SNR 一般
	}
	// SNR < -10 不得分

	// --- 2. RSSI 评分 ---
	if (rssi > -80) {
		score += 3; // 信号极强
	} else if (rssi > -100) {
		score += 2; // 信号良好
	} else if (rssi > -115) {
		score += 1; // 信号一般
	}
	// RSSI < -115 不得分

	// --- 3. 综合判定等级 ---
	// 满分 6 分，最低 0 分
	if (score >= 5) {
		return SIGNAL_EXCELLENT; // 4级：极好
	} else if (score >= 4) {
		return SIGNAL_GOOD; // 3级：良好
	} else if (score >= 2) {
		return SIGNAL_FAIR; // 2级：一般
	} else if (score >= 1) {
		return SIGNAL_BAD; // 1级：差
	} else {
		return SIGNAL_NONE; // 0级：无/无效
	}
}

/* ================================================================
 * 测试统计重置
 * ================================================================ */
static void reset_test_stats(void)
{
	global_params.test_tx_count = 0;
	global_params.test_rx_count = 0;
	global_params.test_rtt_last = 0;
	global_params.test_rtt_min = UINT32_MAX;
	global_params.test_rtt_max = 0;
	global_params.test_rtt_sum = 0;
	global_params.test_last_rx_idx = 0;
	global_params.test_gap_lost = 0;
}

/* ================================================================
 * LoRa 消息类型处理函数
 * ================================================================ */

/* RSSI 响应: [0x03][snr_raw 1B][rssi_raw 1B][test_flag 1B] */
static void handle_rssi_response(const uint8_t *payload, uint16_t payload_len)
{
	if (payload_len < 4) {
		return;
	}

	int8_t raw_snr = (int8_t)payload[1];
	int8_t raw_rssi = (int8_t)payload[2];
	uint8_t test_flag = payload[3];

	/* RSSI 信号等级 */
	int32_t rssi = ((int32_t)raw_rssi - 241) * 3100 + 437887;

	rssi = rssi / 9700;
	SignalQuality_t level = get_lora_signal_level(raw_rssi, raw_snr);

	if (global_params.rssi != (uint8_t)level) {
		global_params.rssi = (uint8_t)level;
		mod_display_lora((uint8_t)level);
		LOG_INF("LoRa RSSI: rssi=%d snr=%d, level=%d", raw_rssi, raw_snr, level);
	}

	/* 保存原始值用于测试模式显示 */
	global_params.test_rssi_raw = raw_rssi;
	global_params.test_snr_raw = raw_snr;

	/* 测试模式切换 */
	bool was_test = global_params.test_mode;

	global_params.test_mode = (test_flag != 0);
	if (global_params.test_mode && !was_test) {
		LOG_INF("Test mode activated");
		reset_test_stats();
		k_event_set(&global_params.event, TEST_EVENT);
		mod_display_test_all(&global_params);
	} else if (!global_params.test_mode && was_test) {
		LOG_INF("Test mode deactivated");
		k_event_clear(&global_params.event, TEST_EVENT);
		mod_display_normal_rows(&global_params);
	}

	/* 测试模式下刷新 Row 1 */
	if (global_params.test_mode) {
		mod_display_test_rssi(raw_rssi, raw_snr);
	}

	rssi_fail_count = 0;
	k_sem_give(&lora_rssi_sem);
}

/* 扫描仪数据 */
static void handle_scanner_data(const uint8_t *payload, uint16_t payload_len)
{
	/* 合并帧: [0x01][flags 1B][ob 2B][laser 4B][cx 4B][cy 4B][cz 4B] = 20B */
	if (payload_len >= 20) {
		scanner_data_t *s = &global_params.scanner;
		uint8_t flags = payload[1];

		s->overbreak_valid = (flags & 0x01) ? 1 : 0;
		s->laser_valid     = (flags & 0x02) ? 1 : 0;
		s->coord_z_valid   = (flags & 0x04) ? 1 : 0;
		s->coord_xy_valid  = (flags & 0x08) ? 1 : 0;
		s->overbreak_value = (int16_t)sys_get_be16(&payload[2]);
		s->laser_distance  = (int32_t)sys_get_be32(&payload[4]);
		s->coord_x         = (int32_t)sys_get_be32(&payload[8]);
		s->coord_y         = (int32_t)sys_get_be32(&payload[12]);
		s->coord_z         = (int32_t)sys_get_be32(&payload[16]);

		mod_display_scanner(s);
		LOG_DBG("Merged scanner: ob=%d laser=%d cx=%d cy=%d cz=%d",
			s->overbreak_value, s->laser_distance,
			s->coord_x, s->coord_y, s->coord_z);
		return;
	} else {
		LOG_ERR("LoRa RX data too short: %d", payload_len);
		return;
	}
}

/* 测试帧回传: [0x02][index 2B BE][timestamp 4B BE] */
static void handle_test_response(const uint8_t *payload, uint16_t payload_len)
{
	if (payload_len >= 7) {
		uint16_t rx_idx = sys_get_be16(payload + 1);
		uint32_t tx_ts = sys_get_be32(payload + 3);
		uint32_t rtt = k_uptime_get_32() - tx_ts;

		global_params.test_rx_count++;
		global_params.test_rtt_last = rtt;
		if (rtt < global_params.test_rtt_min) {
			global_params.test_rtt_min = rtt;
		}
		if (rtt > global_params.test_rtt_max) {
			global_params.test_rtt_max = rtt;
		}
		global_params.test_rtt_sum += rtt;

		if (global_params.test_rx_count > 1) {
			uint16_t gap = rx_idx - global_params.test_last_rx_idx;

			if (gap > 1) {
				global_params.test_gap_lost += gap - 1;
			}
		}
		global_params.test_last_rx_idx = rx_idx;

		uint32_t avg = (uint32_t)(global_params.test_rtt_sum /
					  global_params.test_rx_count);
		uint32_t loss_pct = 0;

		if (global_params.test_tx_count > 0) {
			loss_pct = (global_params.test_tx_count -
				    global_params.test_rx_count) *
			   100 / global_params.test_tx_count;
		}
		LOG_INF("test: idx=%u rtt=%u avg=%u "
			"min=%u max=%u "
			"rx=%u/%u(%u%%) gap_lost=%u",
			rx_idx, rtt, avg,
			global_params.test_rtt_min,
			global_params.test_rtt_max,
			global_params.test_rx_count,
			global_params.test_tx_count, loss_pct,
			global_params.test_gap_lost);

		/* 刷新测试模式显示 */
		if (global_params.test_mode) {
			mod_display_test_loss(global_params.test_gap_lost);
			uint32_t avg = global_params.test_rx_count > 0
				? (uint32_t)(global_params.test_rtt_sum /
					  global_params.test_rx_count) : 0;
			mod_display_test_rtt(global_params.test_rtt_last, avg);
		}
	}
	k_sem_give(&lora_test_sem);
}

static void lora_msg_process_thread(void)
{
	struct lora_data_msg msg;
	uint8_t peek_buf[256];  /* 临时缓冲区，用于 peek 数据 */

	while (true) {
		if (k_msgq_get(&lora_data_msgq, &msg, K_FOREVER) != 0) {
			continue;
		}

		/* 写入环形缓冲区 */
		uint32_t written = ring_buf_put(&lora_rx_rb, msg.data, msg.len);
		if (written != msg.len) {
			LOG_WRN("ring_buf overflow, dropped %u bytes", msg.len - written);
		}

		/* 循环提取完整帧 */
		while (ring_buf_size_get(&lora_rx_rb) > 0) {
			/* Peek 数据到临时缓冲区（最多 256 字节） */
			uint32_t peek_len = ring_buf_peek(&lora_rx_rb, peek_buf, sizeof(peek_buf));
			if (peek_len == 0) {
				break;
			}

			/* 特殊处理: "LoRa Start!" 启动消息 (无帧头) */
			if (memcmp(peek_buf, "LoRa Start!", MIN(peek_len, 11)) == 0) {
				if (peek_len < 11) {
					break;
				}
				LOG_INF("Lora start!");
				uint32_t skip = 11;
				/* 跳过可能的 \r\n 后缀 */
				if (peek_len >= 13 && peek_buf[11] == '\r' &&
				    peek_buf[12] == '\n') {
					skip = 13;
				}
				ring_buf_get(&lora_rx_rb, NULL, skip);
				continue;
			}

			/* 查找帧头 0xAA 0x55 */
			if (peek_buf[0] != 0xAA) {
				ring_buf_get(&lora_rx_rb, NULL, 1);
				continue;
			}
			if (peek_len < 2) {
				break;
			}
			if (peek_buf[1] != 0x55) {
				ring_buf_get(&lora_rx_rb, NULL, 1);
				continue;
			}

			/* 找到帧头, 在后续数据中搜索 \r\n 帧尾 */
			int tail_pos = -1;

			for (int i = 2; i + 1 < peek_len; i++) {
				if (peek_buf[i] == 0x0D && peek_buf[i + 1] == 0x0A) {
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

				if (parse_lora_frame(peek_buf + 2, content_len, &payload,
						     &payload_len)) {
					uint8_t type = payload[0];

					switch (type) {
					case LORA_DATA_RSSI:
						handle_rssi_response(payload, payload_len);
						break;
					case LORA_DATA_HANDLER:
						/* 收到上位机数据, 清除遥测等待标志 */
						k_sem_give(&lora_data_sem);
						handle_scanner_data(payload, payload_len);
						break;
					case LORA_DATA_TEST:
						handle_test_response(payload, payload_len);
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

			/* 丢弃已处理的帧 */
			ring_buf_get(&lora_rx_rb, NULL, total_len);
		}
	}
}
K_THREAD_DEFINE(thread_lora_rx, 2048, lora_msg_process_thread, NULL, NULL, NULL, 8, 0, 0);

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

		/* AT 模式或 shell 手动测试模式: 跳过 RSSI 轮询 */
		if (atomic_get(&lora_current_mode) == LORA_MODE_AT) {
			k_sleep(K_MSEC(LORA_RSSI_PERIOD_MS));
			continue;
		}

		uint32_t t1 = k_uptime_get_32();
		k_sem_reset(&lora_rssi_sem);

		k_mutex_lock(&lora_data_mutex, K_FOREVER);
		bool sent = lora_send_rssi_request();

		if (sent) {
			/* 分段等待, 每 100ms 检查 sleep, 避免休眠时长时间持锁 */
			bool rssi_ok = false;
			for (int i = 0; i < LORA_RSSI_TIMEOUT_MS / 100; i++) {
				if (k_sem_take(&lora_rssi_sem, K_MSEC(100)) == 0) {
					rssi_ok = true;
					break;
				}
				if (global_params.sleeping) {
					break;
				}
			}
			if (rssi_ok) {
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
		k_mutex_unlock(&lora_data_mutex);

		if (rssi_fail_count >= LORA_RSSI_FAIL_MAX) {
			global_params.rssi = 0;
			mod_display_lora(0);
			rssi_fail_count = 0;
		}

		uint32_t diff = k_uptime_get_32() - t1;
		if (diff < LORA_RSSI_PERIOD_MS) {
			k_sleep(K_MSEC(LORA_RSSI_PERIOD_MS - diff));
		}
	}
}
K_THREAD_DEFINE(thread_lora_heart, 1024, lora_rssi_thread, NULL, NULL, NULL, 11, 0, 0);

static void lora_test_tx_thread(void)
{
	static uint16_t test_index = 0;
	uint8_t frame[7]; /* [type 1B][index 2B BE][timestamp 4B BE] */

	frame[0] = LORA_DATA_TEST;

	while (true) {
		if (global_params.sleeping) {
			k_event_wait(&global_params.event, WAKE_EVENT, false, K_FOREVER);
			continue;
		}
		k_event_wait(&global_params.event, LORA_EVENT, false, K_FOREVER);
		k_event_wait(&global_params.event, TEST_EVENT, false, K_FOREVER);
		last_activity_time = k_uptime_get_32();

		uint32_t t1 = k_uptime_get_32();
		sys_put_be16(test_index, &frame[1]);
		sys_put_be32(t1, &frame[3]);

		k_sem_reset(&lora_test_sem);
		k_mutex_lock(&lora_data_mutex, K_FOREVER);
		bool ok = lora_data_send(frame, sizeof(frame));
		if (ok) {
			test_index++;
			global_params.test_tx_count++;
		}
		if (k_sem_take(&lora_test_sem, K_MSEC(2000)) != 0) {
			LOG_WRN("test mode response timeout");
		}
		k_mutex_unlock(&lora_data_mutex);
		uint32_t diff = k_uptime_get_32() - t1;
		if (diff < 200) {
			k_sleep(K_MSEC(200 - diff));
		}
	}
}
K_THREAD_DEFINE(thread_lora_test, 1024, lora_test_tx_thread, NULL, NULL, NULL, 10, 0, 0);

/* ================================================================
 * 网关 ID 设置 — 独立于通信参数
 * ================================================================ */
int lora_set_gw_id(uint32_t gwid)
{
	char cmd[64];

	snprintf(cmd, sizeof(cmd), "AT+GWID=%08X", gwid);
	int ret = lora_set_at_param(cmd, "GWID");

	if (ret == 0) {
		global_params.gwid = gwid;
		LOG_INF("GWID set to %08X", gwid);
	}
	return ret;
}

int lora_set_node_id(uint32_t nid)
{
	char cmd[64];

	snprintf(cmd, sizeof(cmd), "AT+NID=%08X", nid);
	int ret = lora_set_at_param(cmd, "NID");

	if (ret == 0) {
		global_params.nid = nid;
		LOG_INF("Node ID set to %08X", nid);
	}
	return ret;
}

/* ================================================================
 * 通道参数设置 — AT+CH1/AT+CH2/AT+SPD1/AT+SPD2/AT+PNUM, 重启生效
 * ================================================================ */
int lora_set_ch1(uint16_t ch)
{
	char cmd[32];

	snprintf(cmd, sizeof(cmd), "AT+CH1=%d", ch);
	int ret = lora_set_at_param(cmd, "CH1");

	if (ret == 0) {
		global_params.ch1 = ch;
		LOG_INF("CH1 set to %d", ch);
	}
	return ret;
}

int lora_set_ch2(uint16_t ch)
{
	char cmd[32];

	snprintf(cmd, sizeof(cmd), "AT+CH2=%d", ch);
	int ret = lora_set_at_param(cmd, "CH2");

	if (ret == 0) {
		global_params.ch2 = ch;
		LOG_INF("CH2 set to %d", ch);
	}
	return ret;
}

int lora_set_spd1(uint8_t spd)
{
	char cmd[32];

	snprintf(cmd, sizeof(cmd), "AT+SPD1=%d", spd);
	int ret = lora_set_at_param(cmd, "SPD1");

	if (ret == 0) {
		global_params.spd1 = spd;
		LOG_INF("SPD1 set to %d", spd);
	}
	return ret;
}

int lora_set_spd2(uint8_t spd)
{
	char cmd[32];

	snprintf(cmd, sizeof(cmd), "AT+SPD2=%d", spd);
	int ret = lora_set_at_param(cmd, "SPD2");

	if (ret == 0) {
		global_params.spd2 = spd;
		LOG_INF("SPD2 set to %d", spd);
	}
	return ret;
}

int lora_set_pnum(uint8_t pnum)
{
	char cmd[32];

	snprintf(cmd, sizeof(cmd), "AT+PNUM=%d", pnum);
	int ret = lora_set_at_param(cmd, "PNUM");

	if (ret == 0) {
		global_params.pnum = pnum;
		LOG_INF("PNUM set to %d", pnum);
	}
	return ret;
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
	memcpy(tx_data + 1, argv[1], len);

	if (!lora_data_send(tx_data, len + 1)) {
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

static int cmd_gw_mode(const struct shell *ctx, size_t argc, char **argv)
{
	/* lora gw mode [prot] [mode] */
	enum lora_gw_prot prot = LORA_PROT_LG210;
	enum lora_gw_mode mode = LORA_GW_MODE_TRANS;

	if (argc >= 2) {
		if (strcmp(argv[1], "lg210") == 0) {
			prot = LORA_PROT_LG210;
		} else if (strcmp(argv[1], "lg220") == 0) {
			prot = LORA_PROT_LG220;
		} else if (strcmp(argv[1], "node") == 0) {
			prot = LORA_PROT_NODE;
		}
	}
	if (argc >= 3) {
		if (strcmp(argv[2], "net") == 0) {
			mode = LORA_GW_MODE_NETWORK;
		} else if (strcmp(argv[2], "fp") == 0) {
			mode = LORA_GW_MODE_FP;
		}
	}

	struct lora_config cfg = {
		.mode = mode,
		.prot = prot,
		.spd1 = global_params.spd1,
		.ch1 = global_params.ch1,
		.spd2 = global_params.spd2,
		.ch2 = global_params.ch2,
		.pnum = global_params.pnum,
	};

	int ret = lora_configure(&cfg);

	if (ret) {
		shell_error(ctx, "Mode config failed (%d)", ret);
		return ret;
	}
	const char *prot_str = prot == LORA_PROT_LG210  ? "lg210"
			       : prot == LORA_PROT_LG220 ? "lg220" : "node";
	shell_print(ctx, "Mode set: prot=%s mode=%s", prot_str,
		    mode == LORA_GW_MODE_NETWORK ? "net"
		    : mode == LORA_GW_MODE_FP    ? "fp" : "trans");
	return 0;
}

static int cmd_gw_query(const struct shell *ctx, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	struct lora_config cfg;
	int ret = lora_query(&cfg);

	if (ret) {
		shell_error(ctx, "Query failed (%d)", ret);
		return ret;
	}
	const char *prot_str = cfg.prot == LORA_PROT_LG210   ? "lg210"
			       : cfg.prot == LORA_PROT_LG220 ? "lg220" : "node";
	shell_print(ctx, "prot=%s mode=%s spd1=%d ch1=%d spd2=%d ch2=%d pnum=%d",
		    prot_str,
		    cfg.mode == LORA_GW_MODE_NETWORK ? "net"
		    : cfg.mode == LORA_GW_MODE_FP    ? "fp" : "trans",
		    cfg.spd1, cfg.ch1, cfg.spd2, cfg.ch2, cfg.pnum);
	return 0;
}

static int cmd_ch1(const struct shell *ctx, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(ctx, "CH1: spd=%d ch=%d (%dKHz)", global_params.spd1,
			    global_params.ch1, global_params.ch1 * 100);
		return 0;
	}
	/* lora ch1 <spd> <ch> */
	if (argc < 3) {
		shell_error(ctx, "Usage: ch1 <spd 4-11> <ch 4100-5100>");
		return -EINVAL;
	}
	uint8_t spd = (uint8_t)strtol(argv[1], NULL, 10);
	uint16_t ch = (uint16_t)strtol(argv[2], NULL, 10);
	if (spd < 4 || spd > 11) {
		shell_error(ctx, "SPD1 range: 4~11");
		return -EINVAL;
	}
	if (ch < 4100 || ch > 5100) {
		shell_error(ctx, "CH1 range: 4100~5100");
		return -EINVAL;
	}
	int ret = lora_set_spd1(spd);
	if (ret) {
		shell_error(ctx, "Set SPD1 failed (%d)", ret);
		return ret;
	}
	ret = lora_set_ch1(ch);
	if (ret) {
		shell_error(ctx, "Set CH1 failed (%d)", ret);
		return ret;
	}
	shell_print(ctx, "CH1 set: spd=%d ch=%d", spd, ch);
	return 0;
}

static int cmd_ch2(const struct shell *ctx, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(ctx, "CH2: spd=%d ch=%d (%dKHz)", global_params.spd2,
			    global_params.ch2, global_params.ch2 * 100);
		return 0;
	}
	/* lora ch2 <spd> <ch> */
	if (argc < 3) {
		shell_error(ctx, "Usage: ch2 <spd 4-11> <ch 4100-5100>");
		return -EINVAL;
	}
	uint8_t spd = (uint8_t)strtol(argv[1], NULL, 10);
	uint16_t ch = (uint16_t)strtol(argv[2], NULL, 10);
	if (spd < 4 || spd > 11) {
		shell_error(ctx, "SPD2 range: 4~11");
		return -EINVAL;
	}
	if (ch < 4100 || ch > 5100) {
		shell_error(ctx, "CH2 range: 4100~5100");
		return -EINVAL;
	}
	int ret = lora_set_spd2(spd);
	if (ret) {
		shell_error(ctx, "Set SPD2 failed (%d)", ret);
		return ret;
	}
	ret = lora_set_ch2(ch);
	if (ret) {
		shell_error(ctx, "Set CH2 failed (%d)", ret);
		return ret;
	}
	shell_print(ctx, "CH2 set: spd=%d ch=%d", spd, ch);
	return 0;
}

static int cmd_pnum(const struct shell *ctx, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(ctx, "PNUM: %d", global_params.pnum);
		return 0;
	}
	uint8_t pnum = (uint8_t)strtol(argv[1], NULL, 10);
	if (pnum > 2) {
		shell_error(ctx, "PNUM range: 0~2");
		return -EINVAL;
	}
	int ret = lora_set_pnum(pnum);
	if (ret) {
		shell_error(ctx, "Set PNUM failed (%d)", ret);
		return ret;
	}
	shell_print(ctx, "PNUM set to %d", pnum);
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
       SHELL_CMD_ARG(mode, NULL,
        	     "Set protocol and mode\n"
        	     "Usage: mode [prot] [mode]\n"
        	     "  prot: node, lg210 (default), lg220\n"
        	     "  mode: trans (default), fp, net",
        	     cmd_gw_mode, 1, 2),
       SHELL_CMD_ARG(query, NULL, "Query all params", cmd_gw_query, 1, 0),
       SHELL_SUBCMD_SET_END);

static int cmd_test_stats(const struct shell *ctx, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (global_params.test_rx_count > 0) {
		uint32_t avg = (uint32_t)(global_params.test_rtt_sum / global_params.test_rx_count);
		uint32_t loss_pct = 0;
		if (global_params.test_tx_count > 0) {
			loss_pct = (global_params.test_tx_count - global_params.test_rx_count) * 100 /
				   global_params.test_tx_count;
		}
		shell_print(ctx, "tx=%u rx=%u loss=%u%% gap_lost=%u", global_params.test_tx_count,
			    global_params.test_rx_count, loss_pct, global_params.test_gap_lost);
		shell_print(ctx, "rtt: last=%u avg=%u min=%u max=%u (ms)", global_params.test_rtt_last, avg,
			    global_params.test_rtt_min, global_params.test_rtt_max);
	} else {
		shell_print(ctx, "tx=%u rx=0 (no reply yet)", global_params.test_tx_count);
	}
	return 0;
}

static int cmd_test_reset(const struct shell *ctx, size_t argc, char **argv)
{
	ARG_UNUSED(ctx);
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	reset_test_stats();
	if (global_params.test_mode) {
		mod_display_test_loss(0);
		mod_display_test_rtt(0, 0);
	}
	shell_print(ctx, "Test stats reset");
	return 0;
}

void lora_enter_test_mode(void)
{
	if (global_params.test_mode) return;
	global_params.test_mode = true;
	k_event_set(&global_params.event, TEST_EVENT);
	reset_test_stats();
	mod_display_test_all(&global_params);
	LOG_INF("Test mode activated");
}

void lora_exit_test_mode(void)
{
	if (!global_params.test_mode) return;
	global_params.test_mode = false;
	k_event_clear(&global_params.event, TEST_EVENT);
	reset_test_stats();
	mod_display_normal_rows(&global_params);
	LOG_INF("Test mode deactivated");
}

static int cmd_test_on(const struct shell *ctx, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (global_params.test_mode) {
		shell_print(ctx, "Already in test mode");
		return 0;
	}
	lora_enter_test_mode();
	shell_print(ctx, "Test mode activated");
	return 0;
}

static int cmd_test_off(const struct shell *ctx, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (!global_params.test_mode) {
		shell_print(ctx, "Not in test mode");
		return 0;
	}
	lora_exit_test_mode();
	shell_print(ctx, "Test mode deactivated");
	return 0;
}

static int cmd_sleep(const struct shell *ctx, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (global_params.test_mode) {
		shell_error(ctx, "can't sleep in test mode");
		return 0;
	}
	if (global_params.sleeping) {
		shell_warn(ctx, "Already in sleep mode");
	} else {
		system_sleep();
	}

	return 0;
}

static int cmd_log(const struct shell *ctx, size_t argc, char **argv)
{
	uint8_t log = (uint8_t)strtol(argv[1], NULL, 10);
	global_params.log = !!log;
	return 0;
}

static int cmd_display(const struct shell *ctx, size_t argc, char **argv)
{
	if (strncmp(argv[1], "on", 2) == 0) {
		dis_power_enable(true);
	} else if (strncmp(argv[1], "off", 3) == 0) {
		dis_power_enable(false);
	} else {
		shell_warn(ctx, "Usage: display on/off");
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_lora_test_cmds,
	SHELL_CMD_ARG(stats, NULL, "Show test statistics", cmd_test_stats, 1, 0),
	SHELL_CMD_ARG(reset, NULL, "Reset test statistics", cmd_test_reset, 1, 0),
	SHELL_CMD_ARG(on, NULL, "Enter test mode", cmd_test_on, 1, 0),
	SHELL_CMD_ARG(off, NULL, "Exit test mode", cmd_test_off, 1, 0), SHELL_SUBCMD_SET_END);

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_lora_cmds, SHELL_CMD_ARG(send, NULL, "Send data in transparent mode", cmd_send, 2, 0),
	SHELL_CMD_ARG(at, NULL, "Send AT command", cmd_at, 2, 0),
	SHELL_CMD(gw, &sub_lora_gw_cmds, "Gateway operations", NULL),
	SHELL_CMD(test, &sub_lora_test_cmds, "Test RTT/loss stats", NULL),
	SHELL_CMD_ARG(nid, NULL, "Get/set node ID [hex]", cmd_nid, 1, 1),
	SHELL_CMD_ARG(gwid, NULL, "Get/set gateway ID [hex]", cmd_gwid, 1, 1),
	SHELL_CMD_ARG(ch1, NULL, "Get/set CH1 [spd] [ch]", cmd_ch1, 1, 2),
	SHELL_CMD_ARG(ch2, NULL, "Get/set CH2 [spd] [ch]", cmd_ch2, 1, 2),
	SHELL_CMD_ARG(pnum, NULL, "Get/set channel select [0/1/2]", cmd_pnum, 1, 1),
	SHELL_CMD_ARG(sleep, NULL, "system sleep", cmd_sleep, 1, 0),
	SHELL_CMD_ARG(log, NULL, "enable log", cmd_log, 2, 0),
	SHELL_CMD_ARG(display, NULL, "display on/off", cmd_display, 2, 0),
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

	/* 初始化接收环形缓冲区 */
	ring_buf_init(&lora_rx_rb, ASM_BUF_SIZE, lora_rx_buf_data);

	/* 上电 + 复位 + 等待就绪 + 启动 DMA */
	lora_init();

	/* 进入 AT 模式读取模块参数 */
	struct lora_config cfg;

	ret = lora_query(&cfg);
	if (ret == 0) {
		global_params.prot = (uint8_t)cfg.prot;
		global_params.mode = (uint8_t)cfg.mode;
		global_params.spd1 = cfg.spd1;
		global_params.ch1 = cfg.ch1;
		global_params.spd2 = cfg.spd2;
		global_params.ch2 = cfg.ch2;
		global_params.pnum = cfg.pnum;
		LOG_INF("LoRa params: prot=%d mode=%d spd1=%d ch1=%d spd2=%d ch2=%d pnum=%d",
			cfg.prot, cfg.mode, cfg.spd1, cfg.ch1, cfg.spd2, cfg.ch2, cfg.pnum);
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
