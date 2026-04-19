/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * SH1106 OLED 显示驱动 (128x64, I2C)
 * 基于 Zephyr display API, 8x16 ASCII 字体
 *
 * 布局 (4 行 × 16 列):
 *   Row 0 (y=0-15):  连接类型 (CAN/LORA) + 电量/充电状态
 *   Row 1 (y=16-31): X 轴角度
 *   Row 2 (y=32-47): Y 轴角度
 *   Row 3 (y=48-63): 按键状态
 */

#include <common.h>
#include <zephyr/sys/util.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <display.h>
#include <label_icons.h>
#include <battery_icons.h>
#include <signal_icons.h>
#include <mod-can.h>

LOG_MODULE_REGISTER(mod_display, LOG_LEVEL_INF);

/* ================================================================
 * 字体定义 (font_8x16.c)
 * ================================================================ */
#define FONT_8X16_FIRST 0x20
#define FONT_8X16_LAST  0x7E
#define FONT_8X16_COUNT (FONT_8X16_LAST - FONT_8X16_FIRST + 1)

extern const uint8_t font_8x16[FONT_8X16_COUNT][16];

/* ================================================================
 * 显示设备 + 全局状态
 * ================================================================ */
static const struct device *display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static struct display_capabilities capabilities;
static struct display_buffer_descriptor buf_desc;
static K_MUTEX_DEFINE(display_mutex);

/* ================================================================
 * 低级渲染原语
 * ================================================================ */

/* 写入任意位图块 */
static void display_write_buf(int x, int y, int w, int h, const void *data)
{
	buf_desc.buf_size = w * (h / 8);
	buf_desc.height = h;
	buf_desc.width = w;
	buf_desc.pitch = w;
	display_write(display_dev, x, y, &buf_desc, data);
}

/* 渲染单个 8x16 ASCII 字符 */
static void display_char(char ch, int x, int y)
{
	if (ch < FONT_8X16_FIRST || ch > FONT_8X16_LAST) {
		ch = '?';
	}
	display_write_buf(x, y, 8, 16, font_8x16[ch - FONT_8X16_FIRST]);
}

/* 渲染字符串并用空格填充到指定宽度, 覆盖旧内容 */
static void display_str_pad(const char *s, int x, int y, int width)
{
	int end = x + width;

	while (*s && x + 8 <= end && x + 8 <= 128) {
		display_char(*s, x, y);
		x += 8;
		s++;
	}
	/* 用空格填充剩余宽度, 清除旧数据 */
	while (x + 8 <= end && x + 8 <= 128) {
		display_char(' ', x, y);
		x += 8;
	}
}

/* 用空格填充到行尾, 覆盖旧内容 */
void mod_display_clear(void)
{
	for (int j = 0; j < capabilities.y_resolution; j += 16) {
		display_str_pad(" ", 0, j, 128);
	}
}

/* ================================================================
 * 业务显示函数
 * ================================================================ */

/* Row 0 左侧1(16x16): LORA 信号 0/1/2/3/4 */
void mod_display_lora_rssi(uint8_t rssi)
{
	k_mutex_lock(&display_mutex, K_FOREVER);
	if (rssi > 4) rssi = 4;
	display_write_buf(0, 0, SIGNAL_ICON_W, SIGNAL_ICON_H, signal_levels[rssi]);
	k_mutex_unlock(&display_mutex);
}

/* Row 0 左侧2(24x16): 电池图标 */
void mod_display_battery(uint8_t power_level)
{
	k_mutex_lock(&display_mutex, K_FOREVER);

	int idx = power_level >= 75  ? 3
		  : power_level >= 50 ? 2
		  : power_level >= 25 ? 1
				      : 0;
	display_write_buf(16, 0, BATTERY_ICON_W, BATTERY_ICON_H, battery_charging[idx]);

	k_mutex_unlock(&display_mutex);
}

/* Row 0 左侧3(8x16): 连接类型 CAN / LORA */
void mod_display_lora_can(uint8_t connect_type)
{
	k_mutex_lock(&display_mutex, K_FOREVER);
	display_char(' ', 40, 0);
	if (connect_type == CAN_TYPE) {
		display_write_buf(48, 0, LABEL_ICON_W, LABEL_ICON_H, label_can);
	} else if (connect_type == LORA_TYPE) {
		display_write_buf(48, 0, LABEL_ICON_W, LABEL_ICON_H, label_lora);
	} else {
		display_char(' ', 48, 0);
	}
	display_char(' ', 56, 0);
	k_mutex_unlock(&display_mutex);
}

/* Row 0 左侧4: 节点ID */
void mod_display_lora_nid(uint32_t nid)
{
	char line[32] = {0};
	k_mutex_lock(&display_mutex, K_FOREVER);
	snprintf(line, sizeof(line), "%08X", nid);
	display_str_pad(line, 64, 0, 64);
	k_mutex_unlock(&display_mutex);
}


/* Row 1: 激光距离 + 超欠挖 (来自扫描仪 CAN 数据) */
void mod_display_scanner(const scanner_data_t *s)
{
	char line[32] = {0};

	k_mutex_lock(&display_mutex, K_FOREVER);

	if (s->overbreak_valid == 1) {
		snprintf(line, sizeof(line), "OB:%-5ld m", (long)s->overbreak_value);
	} else {
		snprintf(line, sizeof(line), "OB: --- ");
	}
	display_str_pad(line, 0, 16, 64);

	if (s->laser_valid == 1) {
		snprintf(line, sizeof(line), "Dis:%-5ld m", (long)s->laser_distance);
	} else {
		snprintf(line, sizeof(line), "Dis: ---");
	}
	display_str_pad(line, 64, 16, 128);

	k_mutex_unlock(&display_mutex);
}

/* Row 2: X/Y 角度 (紧凑格式, 整数运算避免 %f 代码膨胀) */
void mod_display_handler_xy(int x, int y)
{
	char line[32];
	/* 角度范围 ±20.0° → abs 值 0~200, uint16 足够且消除 snprintf 截断警告 */
	uint16_t ax = (uint16_t)(x < 0 ? -x : x);
	uint16_t ay = (uint16_t)(y < 0 ? -y : y);

	k_mutex_lock(&display_mutex, K_FOREVER);
	snprintf(line, sizeof(line), "X:%c%u.%u ", x < 0 ? '-' : '+', ax / 10, ax % 10);
	display_str_pad(line, 0, 32, 64);
	snprintf(line, sizeof(line), "Y:%c%u.%u ", y < 0 ? '-' : '+', ay / 10, ay % 10);
	display_str_pad(line, 64, 32, 64);
	k_mutex_unlock(&display_mutex);
}

/* Row 3: 按键状态 */
void mod_display_handler_button(uint8_t h_button)
{
	char line[17];

	k_mutex_lock(&display_mutex, K_FOREVER);
	snprintf(line, sizeof(line), "BTN: %s", h_button ? "ON " : "OFF");
	display_str_pad(line, 0, 48, 128);
	k_mutex_unlock(&display_mutex);
}

/* 全屏刷新 */
void mod_display_all(const gloval_params_t *params)
{
	mod_display_lora_rssi(params->rssi);
	mod_display_battery(params->power_level);
	mod_display_lora_can(params->connect_type);
	mod_display_lora_nid(params->nid);

	mod_display_scanner(&params->scanner);
	mod_display_handler_xy(params->x_degree, params->y_degree);
	mod_display_handler_button(params->h_button);
}
/* ================================================================
 * 初始化
 * ================================================================ */
gloval_params_t global_params;

int mod_display_init(void)
{
	global_params.can_heart_time = CAN_HEART_TIME;
	global_params.connect_type = CAN_TYPE;
	k_event_init(&global_params.event);

	if (!device_is_ready(display_dev)) {
		LOG_ERR("Device %s not found.", display_dev->name);
		return -1;
	}

	display_get_capabilities(display_dev, &capabilities);
	LOG_INF("Display %dx%d", capabilities.x_resolution, capabilities.y_resolution);

	display_blanking_off(display_dev);
	mod_display_all(&global_params);

	return 0;
}

SYS_INIT(mod_display_init, APPLICATION, 1);

