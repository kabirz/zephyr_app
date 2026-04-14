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
static const struct device *display_dev =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static struct display_capabilities capabilities;
static struct display_buffer_descriptor buf_desc;
static K_MUTEX_DEFINE(display_mutex);

/* ================================================================
 * 初始化
 * ================================================================ */
int mod_display_init(void)
{
	if (!device_is_ready(display_dev)) {
		LOG_ERR("Device %s not found.", display_dev->name);
		return -1;
	}

	display_get_capabilities(display_dev, &capabilities);
	LOG_INF("Display %dx%d", capabilities.x_resolution,
		capabilities.y_resolution);

	display_clear(display_dev);
	display_blanking_off(display_dev);

	return 0;
}

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

/* 渲染字符串, 返回末尾 x 坐标 */
static int display_str(const char *s, int x, int y)
{
	while (*s && x + 8 <= 128) {
		display_char(*s, x, y);
		x += 8;
		s++;
	}
	return x;
}

/* 用空格填充到行尾, 覆盖旧内容 */
static void display_clear_rest(int x, int y)
{
	uint8_t blank[8] = {0};

	while (x + 8 <= 128) {
		display_write_buf(x, y, 8, 16, blank);
		x += 8;
	}
}

/* ================================================================
 * 16x16 中文点阵 (保留兼容)
 * ================================================================ */
static const uint8_t bitmap_bytes[][32] = {
	/* 你 */
	{0x02,0x04,0x1F,0xE0,0x02,0x04,0x18,0xF0,0x10,0x13,0x10,0x10,
	 0x14,0x18,0x00,0x00,0x00,0x00,0xFF,0x00,0x00,0x10,0x20,0xC2,
	 0x01,0xFE,0x00,0x80,0x60,0x30,0x00,0x00},
	/* 好 */
	{0x08,0x08,0x0F,0xF8,0x08,0x0F,0x01,0x41,0x41,0x41,0x47,0x49,
	 0x51,0x63,0x01,0x00,0x02,0x44,0xA8,0x10,0x28,0xC6,0x00,0x00,
	 0x02,0x01,0xFE,0x00,0x00,0x00,0x00,0x00},
};

void mod_display_16x16(const uint8_t *data, int x, int y)
{
	buf_desc.buf_size = 32;
	buf_desc.height = 16;
	buf_desc.width = 16;
	buf_desc.pitch = 16;
	display_write(display_dev, x, y, &buf_desc, data);
}

void mod_display_demo(void)
{
	for (int h = 0; h < capabilities.y_resolution; h += 16) {
		for (int w = 0; w < capabilities.x_resolution; w += 16) {
			mod_display_16x16(bitmap_bytes[(w % 32) == 16], w, h);
		}
	}
}

/* ================================================================
 * 电池图标位图 — 水平方向 (24x16, 5 级: 0~4 格)
 *
 * 布局 (cols 0-23, rows 0-15):
 *   外框: 顶/底 rows 1,14  正极凸起 cols 18-20 rows 4-11
 *   填充区: cols 2-16, rows 2-13
 *   4 格: seg1=cols 2-4, seg2=cols 6-8, seg3=cols 10-12, seg4=cols 14-16
 *          每格 3 cols 宽, 格间距 1 col (cols 5,9,13)
 *
 * 取模: MONO_VTILED, MSB 上高位
 * 字节序: page0[24] page1[24], 每字节 1 列, 共 48 字节
 * ================================================================ */
#define BATTERY_ICON_W  24
#define BATTERY_ICON_H  16
#define BATTERY_ICON_SZ (BATTERY_ICON_W * BATTERY_ICON_H / 8) /* 48 */

static const uint8_t battery_icons[5][BATTERY_ICON_SZ] = {
	/* 0 格 — 空壳 */
	{0x00,0x7F,0x40,0x40,0x40,0x40,0x40,0x40,0x40,0x40,0x40,0x40,
	 0x40,0x40,0x40,0x40,0x40,0x7F,0x0F,0x0F,0x0F,0x00,0x00,0x00,
	 0x00,0xFE,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,0x02,
	 0x02,0x02,0x02,0x02,0x02,0xFE,0xE0,0xE0,0xE0,0x00,0x00,0x00},
	/* 1 格 — seg1 (cols 2-4) */
	{0x00,0x7F,0x7F,0x7F,0x7F,0x40,0x40,0x40,0x40,0x40,0x40,0x40,
	 0x40,0x40,0x40,0x40,0x40,0x7F,0x0F,0x0F,0x0F,0x00,0x00,0x00,
	 0x00,0xFE,0xFE,0xFE,0xFE,0x02,0x02,0x02,0x02,0x02,0x02,0x02,
	 0x02,0x02,0x02,0x02,0x02,0xFE,0xE0,0xE0,0xE0,0x00,0x00,0x00},
	/* 2 格 — seg1 + seg2 (cols 2-4, 6-8) */
	{0x00,0x7F,0x7F,0x7F,0x7F,0x40,0x7F,0x7F,0x7F,0x40,0x40,0x40,
	 0x40,0x40,0x40,0x40,0x40,0x7F,0x0F,0x0F,0x0F,0x00,0x00,0x00,
	 0x00,0xFE,0xFE,0xFE,0xFE,0x02,0xFE,0xFE,0xFE,0x02,0x02,0x02,
	 0x02,0x02,0x02,0x02,0x02,0xFE,0xE0,0xE0,0xE0,0x00,0x00,0x00},
	/* 3 格 — seg1-3 (cols 2-4, 6-8, 10-12) */
	{0x00,0x7F,0x7F,0x7F,0x7F,0x40,0x7F,0x7F,0x7F,0x40,0x7F,0x7F,
	 0x7F,0x40,0x40,0x40,0x40,0x7F,0x0F,0x0F,0x0F,0x00,0x00,0x00,
	 0x00,0xFE,0xFE,0xFE,0xFE,0x02,0xFE,0xFE,0xFE,0x02,0xFE,0xFE,
	 0xFE,0x02,0x02,0x02,0x02,0xFE,0xE0,0xE0,0xE0,0x00,0x00,0x00},
	/* 4 格 — 满电 (cols 2-4, 6-8, 10-12, 14-16) */
	{0x00,0x7F,0x7F,0x7F,0x7F,0x40,0x7F,0x7F,0x7F,0x40,0x7F,0x7F,
	 0x7F,0x40,0x7F,0x7F,0x7F,0x7F,0x0F,0x0F,0x0F,0x00,0x00,0x00,
	 0x00,0xFE,0xFE,0xFE,0xFE,0x02,0xFE,0xFE,0xFE,0x02,0xFE,0xFE,
	 0xFE,0x02,0xFE,0xFE,0xFE,0xFE,0xE0,0xE0,0xE0,0x00,0x00,0x00},
};

/* ================================================================
 * 业务显示函数
 * ================================================================ */

/* Row 0 左侧: 连接类型 CAN / LORA */
void mod_display_lora_can(const gloval_params_t *params)
{
	k_mutex_lock(&display_mutex, K_FOREVER);
	const char *type =
		(params->connect_type == LORA_TYPE) ? "LORA" : "CAN ";
	display_str(type, 0, 0);
	k_mutex_unlock(&display_mutex);
}

/* Row 0 右侧: 电池图标 + 百分比 + 充电状态 */
void mod_display_battery(const gloval_params_t *params)
{
	char line[17];

	k_mutex_lock(&display_mutex, K_FOREVER);

	/* 选择对应格数的图标 */
	int idx = params->power_level >= 100 ? 4 :
		  params->power_level >= 75  ? 3 :
		  params->power_level >= 50  ? 2 :
		  params->power_level >= 25  ? 1 : 0;
	display_write_buf(40, 0, BATTERY_ICON_W, BATTERY_ICON_H,
			  battery_icons[idx]);

	/* 图标右侧: 百分比 + 充电状态 */
	const char *status;

	switch (params->battery_status) {
	case BATTERY_STATUS_FULL:
		status = "FUL";
		break;
	case BATTERY_STATUS_CHARGING:
		status = "CHG";
		break;
	case BATTERY_STATUS_DISCHARGING:
		status = "DIS";
		break;
	default:
		status = "UNK";
		break;
	}

	snprintf(line, sizeof(line), "%3d%% %s ", params->power_level, status);

	/* 清除图标右侧区域后写入 */
	display_clear_rest(64, 0);
	display_str(line, 64, 0);

	k_mutex_unlock(&display_mutex);
}

/* Row 1: X 轴角度 */
void mod_display_handler_x(const gloval_params_t *params)
{
	char line[17];

	k_mutex_lock(&display_mutex, K_FOREVER);
	snprintf(line, sizeof(line), "X:%+5.1f      ", (double)params->x_degree);
	display_str(line, 0, 16);
	k_mutex_unlock(&display_mutex);
}

/* Row 2: Y 轴角度 */
void mod_display_handler_y(const gloval_params_t *params)
{
	char line[17];

	k_mutex_lock(&display_mutex, K_FOREVER);
	snprintf(line, sizeof(line), "Y:%+5.1f      ", (double)params->y_degree);
	display_str(line, 0, 32);
	k_mutex_unlock(&display_mutex);
}

/* Row 3: 按键状态 */
void mod_display_handler_button(const gloval_params_t *params)
{
	char line[17];

	k_mutex_lock(&display_mutex, K_FOREVER);
	snprintf(line, sizeof(line), "BTN:%s        ",
		 params->h_button ? "ON " : "OFF");
	display_str(line, 0, 48);
	k_mutex_unlock(&display_mutex);
}

/* 全屏刷新 */
void mod_display_all(const gloval_params_t *params)
{
	mod_display_lora_can(params);
	mod_display_battery(params);
	mod_display_handler_x(params);
	mod_display_handler_y(params);
	mod_display_handler_button(params);
}
