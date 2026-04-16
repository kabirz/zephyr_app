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
	{
	 0x00, 0xFE, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
	 0x02, 0x02, 0x02, 0x02, 0x02, 0xFE, 0xE0, 0xE0, 0xE0, 0x00, 0x00, 0x00,
	 0x00, 0x7F, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,
	 0x40, 0x40, 0x40, 0x40, 0x40, 0x7F, 0x0F, 0x0F, 0x0F, 0x00, 0x00, 0x00,
	},
	/* 1 格 — seg1 (cols 2-4) */
	{
	 0x00, 0xFE, 0xFE, 0xFE, 0xFE, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02,
	 0x02, 0x02, 0x02, 0x02, 0x02, 0xFE, 0xE0, 0xE0, 0xE0, 0x00, 0x00, 0x00,
	 0x00, 0x7F, 0x7F, 0x7F, 0x7F, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40,
	 0x40, 0x40, 0x40, 0x40, 0x40, 0x7F, 0x0F, 0x0F, 0x0F, 0x00, 0x00, 0x00,
	},
	/* 2 格 — seg1 + seg2 (cols 2-4, 6-8) */
	{
	 0x00, 0xFE, 0xFE, 0xFE, 0xFE, 0x02, 0xFE, 0xFE, 0xFE, 0x02, 0x02, 0x02,
	 0x02, 0x02, 0x02, 0x02, 0x02, 0xFE, 0xE0, 0xE0, 0xE0, 0x00, 0x00, 0x00,
	 0x00, 0x7F, 0x7F, 0x7F, 0x7F, 0x40, 0x7F, 0x7F, 0x7F, 0x40, 0x40, 0x40,
	 0x40, 0x40, 0x40, 0x40, 0x40, 0x7F, 0x0F, 0x0F, 0x0F, 0x00, 0x00, 0x00,
	},
	/* 3 格 — seg1-3 (cols 2-4, 6-8, 10-12) */
	{
	 0x00, 0xFE, 0xFE, 0xFE, 0xFE, 0x02, 0xFE, 0xFE, 0xFE, 0x02, 0xFE, 0xFE,
	 0xFE, 0x02, 0x02, 0x02, 0x02, 0xFE, 0xE0, 0xE0, 0xE0, 0x00, 0x00, 0x00,
	 0x00, 0x7F, 0x7F, 0x7F, 0x7F, 0x40, 0x7F, 0x7F, 0x7F, 0x40, 0x7F, 0x7F,
	 0x7F, 0x40, 0x40, 0x40, 0x40, 0x7F, 0x0F, 0x0F, 0x0F, 0x00, 0x00, 0x00,
	},
	/* 4 格 — 满电 (cols 2-4, 6-8, 10-12, 14-16) */
	{
	 0x00, 0xFE, 0xFE, 0xFE, 0xFE, 0x02, 0xFE, 0xFE, 0xFE, 0x02, 0xFE, 0xFE,
	 0xFE, 0x02, 0xFE, 0xFE, 0xFE, 0xFE, 0xE0, 0xE0, 0xE0, 0x00, 0x00, 0x00,
	 0x00, 0x7F, 0x7F, 0x7F, 0x7F, 0x40, 0x7F, 0x7F, 0x7F, 0x40, 0x7F, 0x7F,
	 0x7F, 0x40, 0x7F, 0x7F, 0x7F, 0x7F, 0x0F, 0x0F, 0x0F, 0x00, 0x00, 0x00,
	},
};

/* ================================================================
 * 业务显示函数
 * ================================================================ */

/* Row 0 左侧1: LORA 信号 0/1/2/3/4/5 */
void mod_display_lora_rssi(uint8_t rssi)
{
	k_mutex_lock(&display_mutex, K_FOREVER);
	if (rssi > 5) rssi = 5;
	char rssi_str[2];
	rssi_str[0] = '0' + rssi;
	rssi_str[1] = '\0';
	display_str_pad(rssi_str, 0, 0, 16);
	k_mutex_unlock(&display_mutex);
}

/* Row 0 左侧2: 电池图标 */
void mod_display_battery(uint8_t power_level)
{
	k_mutex_lock(&display_mutex, K_FOREVER);

	/* 选择对应格数的图标 */
	int idx = power_level >= 80  ? 4
		  : power_level >= 60 ? 3
		  : power_level >= 40 ? 2
		  : power_level >= 20 ? 1
				      : 0;
	display_write_buf(16, 0, BATTERY_ICON_W, BATTERY_ICON_H, battery_icons[idx]);

	k_mutex_unlock(&display_mutex);
}

/* Row 0 左侧3: 连接类型 CAN / LORA */
void mod_display_lora_can(uint8_t connect_type)
{
	k_mutex_lock(&display_mutex, K_FOREVER);
	const char *type = (connect_type == LORA_TYPE) ? " L" : " C";
	display_str_pad(type, 40, 0, 24);
	k_mutex_unlock(&display_mutex);
}

/* Row 0 左侧4: 网关ID */
void mod_display_lora_gwid(uint32_t gwid)
{
	char line[32] = {0};
	k_mutex_lock(&display_mutex, K_FOREVER);
	snprintf(line, sizeof(line), "%08X", gwid);
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
	mod_display_lora_gwid(params->gwid);

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

