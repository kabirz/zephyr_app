/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * LoRa UART 驱动 — WH-L101-L (有人物联网)
 * 基于 Zephyr UART async API + DMA
 *
 * 双模式设计:
 *   数据模式 (LORA_MODE_DATA): 透传收发, DMA 双缓冲, rx_timeout 判定帧边界
 *   AT 模式   (LORA_MODE_AT):  +++a 握手进入, 一问一答, 同步等待响应
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
#include <power.h>
#include <lora.h>

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

/* ================================================================
 * DMA 双缓冲
 * ================================================================ */
#define LORA_BUF_SIZE 256

static uint8_t rx_buf_a[LORA_BUF_SIZE];
static uint8_t rx_buf_b[LORA_BUF_SIZE];
static uint8_t *rx_next_buf = rx_buf_b;
static uint32_t node_id;

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
	uint8_t tx_data[32];
	int offset = 0;

	if (atomic_get(&lora_current_mode) != LORA_MODE_DATA) {
		LOG_WRN("Not in data mode");
		return false;
	}
	/* 节点ID */
	snprintf(tx_data, 9, "%08x", node_id);
	offset += 8;

	/* 需要发送的数据 */
	memcpy(tx_data + offset, data, len);
	offset += len;

	/* CRC16 */
	*(uint16_t *)(tx_data + offset) = crc16_ccitt(0, tx_data, offset);

	offset += 2;

	/* 等待模块空闲 (HOSTWAKE 低 = 模块未在无线收发) */
	int retry = 10;
	while (lora_get_hostwake_status() && retry-- > 0) {
		k_msleep(5);
	}
	if (retry <= 0) {
		LOG_WRN("LoRa module busy");
		return false;
	}

	int ret = lora_async_tx(tx_data, offset);

	return (ret == 0);
}

/* ================================================================
 * 遥测数据帧打包发送 — 与 CAN 手柄状态帧 (0x1E3) 格式一致
 *
 * 帧格式 (8 字节, 大端序):
 *   [0-1] X 角度 (int16_t BE, 0.1° 单位)
 *   [2-3] Y 角度 (int16_t BE, 0.1° 单位)
 *   [4]   按键 (bit0: btnHandler 反转, 按下=0, 松开=1)
 *   [5-7] reserved (0xFF)
 * ================================================================ */
bool lora_send_telemetry(const gloval_params_t *params)
{
	uint8_t frame[8];

	/* 大端序写入角度 */
	sys_put_be16((uint16_t)params->x_degree, &frame[0]);
	sys_put_be16((uint16_t)params->y_degree, &frame[2]);

	/* 按键: btnHandler 反转逻辑 (按下=0, 松开=1) */
	frame[4] = params->h_button ? 0x00 : 0x01;
	frame[5] = 0xFF;
	frame[6] = 0xFF;
	frame[7] = 0xFF;

	return lora_data_send(frame, sizeof(frame));
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
 * AT 模式退出 — AT+ENTM
 * ================================================================ */
int lora_exit_at(void)
{
	/* 发送退出指令, 等待 +OK */
	at_resp_len = 0;
	k_sem_reset(&at_resp_sem);
	lora_async_tx((const uint8_t *)"AT+ENTM\r\n", 9);
	k_sem_take(&at_resp_sem, K_SECONDS(3));

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
	if (strncmp("AT+Z", cmd, 4) == 0) {
		LOG_INF("reboot WH-L101-L");
		atomic_set(&lora_current_mode, LORA_MODE_DATA);
	}

	if (resp && resp_size > 0) {
		strncpy(resp, (const char *)at_resp_buf, resp_size - 1);
		resp[resp_size - 1] = '\0';
	}

	return (at_resp_len > 0) ? 0 : -ETIMEDOUT;
}

/* ================================================================
 * LG210 网关配置 — LORAPROT + SPD + CH + 保存 + 重启
 * ================================================================ */
int lora_gw_configure(const struct lora_gw_config *cfg)
{
	struct lora_gw_config defaults = {
		.mode = LORA_GW_MODE_TRANS,
		.spd = 10,
		.ch = 72,
		.nid = 0,
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

	/* 选择 LG210 协议 */
	ret = lora_send_at("AT+LORAPROT=LG210", resp, sizeof(resp), 2000);
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

	/* 组网模式: 设置网关 ID */
	if (cfg->mode == LORA_GW_MODE_NETWORK) {
		snprintf(cmd, sizeof(cmd), "AT+NID=%d", cfg->nid);
		ret = lora_send_at(cmd, resp, sizeof(resp), 2000);
		if (ret) {
			LOG_ERR("Set NID failed");
			goto out;
		}
	}

	/* 保存为出厂默认 */
	lora_send_at("AT+CFGTF", resp, sizeof(resp), 2000);

	/* 重启使配置生效 */
	lora_send_at("AT+Z", resp, sizeof(resp), 2000);

	/* 模块重启后直接进入透传模式, 恢复数据模式 RX */
	k_msleep(1000);
	lora_rx_disable_sync();
	atomic_set(&lora_current_mode, LORA_MODE_DATA);
	uart_rx_enable(uart_dev, rx_buf_a, LORA_BUF_SIZE, LORA_DATA_RX_TIMEOUT);
	k_mutex_unlock(&lora_mode_mutex);

	LOG_INF("Gateway configured: mode=%s spd=%d ch=%d nid=%d",
		cfg->mode == LORA_GW_MODE_NETWORK ? "network" : "trans", cfg->spd, cfg->ch,
		cfg->nid);
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
	 * 例: \r\n+NID:5\r\n\r\nOK\r\n
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
 * LG210 网关参数查询 — 读取 NID
 * ================================================================ */
static int parse_at_nid(const char *resp)
{
	/* AT 查询响应格式: \r\n+CMD:<value>\r\n\r\nOK\r\n
	 * 例: \r\n+NID:5\r\n\r\nOK\r\n
	 *     \r\n+SPD:10\r\n\r\nOK\r\n
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

	/* 查询网关 ID — 有值则为组网模式, 否则为透传模式 */
	cfg->nid = 0;
	cfg->mode = LORA_GW_MODE_TRANS;
	ret = lora_send_at("AT+NID", resp, sizeof(resp), 2000);
	if (ret == 0) {
		int nid_val = parse_at_nid(resp);
		if (nid_val > 0) {
			cfg->nid = (uint32_t)nid_val;
			cfg->mode = LORA_GW_MODE_NETWORK;
		}
	} else {
		LOG_WRN("Query NID failed");
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
 * 数据接收处理线程 — 从 msgq 取帧, 上报应用层
 * ================================================================ */
static void lora_msg_process_thread(void)
{
	struct lora_data_msg msg;

	while (true) {
		if (k_msgq_get(&lora_data_msgq, &msg, K_FOREVER) == 0) {
			LOG_HEXDUMP_INF(msg.data, msg.len, "LoRa RX:");
			/* TODO: 解析透传数据, 上报应用层 */
		}
	}
}
K_THREAD_DEFINE(lora_msg, 1024, lora_msg_process_thread, NULL, NULL, NULL, 12, 0, 0);

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

	size_t len = strlen(argv[1]);

	if (!lora_data_send((const uint8_t *)argv[1], len)) {
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

	snprintf(at_cmd, sizeof(at_cmd), "AT+%s\r\n", argv[1]);
	int ret = lora_send_at(at_cmd, resp, sizeof(resp), 2000);

	if (ret == 0) {
		shell_print(ctx, "%s", resp);
	} else {
		shell_error(ctx, "AT command timeout");
	}
	return ret;
}

static int cmd_exit(const struct shell *ctx, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	if (atomic_get(&lora_current_mode) != LORA_MODE_AT) {
		shell_warn(ctx, "Not in AT mode");
		return 0;
	}
	return lora_exit_at();
}

static int cmd_gw_config(const struct shell *ctx, size_t argc, char **argv)
{
	struct lora_gw_config cfg = {
		.mode = LORA_GW_MODE_TRANS,
		.spd = 10,
		.ch = 72,
		.nid = 0,
	};

	/* lora gw config [mode] [spd] [ch] [nid]
	 * mode: "trans" (默认) 或 "net"
	 */
	if (argc >= 2) {
		if (strcmp(argv[1], "net") == 0) {
			cfg.mode = LORA_GW_MODE_NETWORK;
		}
	}
	if (argc >= 3) {
		cfg.spd = (uint8_t)strtol(argv[2], NULL, 10);
	}
	if (argc >= 4) {
		cfg.ch = (uint8_t)strtol(argv[3], NULL, 10);
	}
	if (argc >= 5) {
		cfg.nid = (uint16_t)strtol(argv[4], NULL, 16);
	}

	/* 组网模式必须提供网关 ID */
	if (cfg.mode == LORA_GW_MODE_NETWORK && cfg.nid == 0) {
		shell_error(ctx, "Network mode requires NID > 0");
		return -EINVAL;
	}

	int ret = lora_gw_configure(&cfg);

	if (ret) {
		shell_error(ctx, "Gateway config failed (%d)", ret);
		return ret;
	}
	shell_print(ctx, "Gateway configured: mode=%s spd=%d ch=%d nid=%x",
		    cfg.mode == LORA_GW_MODE_NETWORK ? "net" : "trans", cfg.spd, cfg.ch, cfg.nid);
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
	shell_print(ctx, "Gateway params: mode=%s spd=%d ch=%d nid=%x",
		    cfg.mode == LORA_GW_MODE_NETWORK ? "net" : "trans", cfg.spd, cfg.ch, cfg.nid);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_lora_gw_cmds,
			       SHELL_CMD_ARG(config, NULL,
					     "Configure gateway params and reboot\n"
					     "Usage: config [trans|net] [spd] [ch] [nid]\n"
					     "  trans  Transparent mode (default)\n"
					     "  net    Network mode (requires nid)",
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
			       SHELL_CMD_ARG(exit, NULL, "Exit AT mode, return to data mode",
					     cmd_exit, 1, 0),
			       SHELL_CMD(gw, &sub_lora_gw_cmds, "LG210 gateway operations", NULL),
			       SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(lora, &sub_lora_cmds, "LoRa WH-L101-L commands", NULL);
#endif /* CONFIG_SHELL */

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

	/* 上电 + 硬件复位 */
	lora_power_enable(true);
	gpio_pin_set_dt(&lora_reset_pin, 0);
	k_msleep(10);
	gpio_pin_set_dt(&lora_reset_pin, 1);
	k_msleep(500);

	/* HOSTWAKE 初始化为输入 */
	gpio_pin_configure_dt(&lora_hostwake_pin, GPIO_INPUT | GPIO_PULL_UP);

	/* 注册 async 回调 */
	uart_callback_set(uart_dev, lora_uart_cb, NULL);

	/* 启动 DMA 接收 — 数据模式, 20ms 帧间超时 */
	ret = uart_rx_enable(uart_dev, rx_buf_a, LORA_BUF_SIZE, LORA_DATA_RX_TIMEOUT);
	if (ret != 0) {
		LOG_ERR("Failed to enable UART RX: %d", ret);
		return ret;
	}

	/* 进入 AT 模式读取模块参数 (NID/SPD/CH) */
	struct lora_gw_config cfg;

	ret = lora_gw_query(&cfg);
	if (ret == 0) {
		node_id = cfg.nid;
		LOG_INF("LoRa params: mode=%s nid=0x%08x spd=%d ch=%d",
			cfg.mode == LORA_GW_MODE_NETWORK ? "net" : "trans", node_id, cfg.spd,
			cfg.ch);
	} else {
		LOG_WRN("LoRa param query failed (%d), using defaults", ret);
	}
	LOG_INF("LoRa WH-L101-L initialized (async DMA, buf=%d, timeout=%dms)", LORA_BUF_SIZE,
		LORA_DATA_RX_TIMEOUT);
	return 0;
}

SYS_INIT(lora_serial_init, APPLICATION, 10);
