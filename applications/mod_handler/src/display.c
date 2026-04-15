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

LOG_MODULE_REGISTER(mod_display, LOG_LEVEL_INF);

/* ================================================================
 * 字体定义 (font_8x16.c)
 * ================================================================ */
#define FONT_8X16_FIRST 0x20
#define FONT_8X16_LAST  0x7E
#define FONT_8X16_COUNT (FONT_8X16_LAST - FONT_8X16_FIRST + 1)

extern const font8x16_t font_8x16[];
extern int font_8x16_len;

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
	for (int i = 0; i < font_8x16_len; i++) {
		if (font_8x16[i].val == ch) {
			display_write_buf(x, y, 8, 16, font_8x16[i].data);
			return;
		}

	}
	LOG_ERR("without '%c' in table", ch);
	display_write_buf(x, y, 8, 16, font_8x16[0].data);
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
void mod_display_clear(void)
{
	uint8_t blank[16] = {0};
	for (int i = 0; i < capabilities.x_resolution; i += 8) {
		for (int j = 0; j < capabilities.y_resolution; j += 16) {
			display_write_buf(i, j, 8, 16, blank);
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
	k_mutex_lock(&display_mutex, K_FOREVER);

	/* 选择对应格数的图标 */
	int idx = params->power_level >= 100 ? 4 :
		  params->power_level >= 75  ? 3 :
		  params->power_level >= 50  ? 2 :
		  params->power_level >= 25  ? 1 : 0;
	display_write_buf(40, 0, BATTERY_ICON_W, BATTERY_ICON_H,
			  battery_icons[idx]);

	k_mutex_unlock(&display_mutex);
}

/* Row 1: 激光距离 + 超欠挖 (来自扫描仪 CAN 数据) */
void mod_display_scanner(const gloval_params_t *params)
{
	char line[17] = {0};
	const scanner_data_t *s = &params->scanner;

	k_mutex_lock(&display_mutex, K_FOREVER);

	if (s->laser_valid == 1) {
		snprintf(line, sizeof(line), "D:%-5ld", (long)s->laser_distance);
	} else {
		snprintf(line, sizeof(line), "D:---  ");
	}
	display_str(line, 0, 16);

	if (s->overbreak_valid == 1) {
		snprintf(line, sizeof(line), "OB:%-5ld", (long)s->overbreak_value);
	} else {
		snprintf(line, sizeof(line), "OB:--- ");
	}
	display_str(line, 80, 16);

	k_mutex_unlock(&display_mutex);
}

/* Row 2: X/Y 角度 (紧凑格式, 整数运算避免 %f 代码膨胀) */
void mod_display_handler_xy(const gloval_params_t *params)
{
	char line[17] = {0};
	int x = params->x_degree, y = params->y_degree;
	int ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;

	k_mutex_lock(&display_mutex, K_FOREVER);
	snprintf(line, sizeof(line), "X:%c%d.%d Y:%c%d.%d",
		 x < 0 ? '-' : '+', ax / 10, ax % 10,
		 y < 0 ? '-' : '+', ay / 10, ay % 10);
	display_str(line, 0, 32);
	k_mutex_unlock(&display_mutex);
}

/* Row 3: 按键状态 */
void mod_display_handler_button(const gloval_params_t *params)
{
	char line[17];

	k_mutex_lock(&display_mutex, K_FOREVER);
	snprintf(line, sizeof(line), "BTN:%s",
		 params->h_button ? "ON " : "OFF");
	display_str(line, 0, 48);
	k_mutex_unlock(&display_mutex);
}

/* 全屏刷新 */
void mod_display_all(const gloval_params_t *params)
{
	mod_display_lora_can(params);
	mod_display_battery(params);
	mod_display_scanner(params);
	mod_display_handler_xy(params);
	mod_display_handler_button(params);
}
