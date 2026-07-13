/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * GPIO 按键检测 + 外设电源管理 + CAN/2.4G 切换 + 系统休眠/唤醒
 * 按键防抖: ISR → k_work_delayable, 延时读 GPIO 电平确认
 * 电源控制: CAN/2.4G(nRF24)/显示/手柄 4 路 GPIO 独立使能
 */

#include <stdlib.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/logging/log.h>
#include <common.h>
#include <display.h>
#include <mod-can.h>
#include <rf24.h>
#include <mod-gpio.h>
#include <persist.h>

LOG_MODULE_REGISTER(power_gpio, LOG_LEVEL_INF);

/**
 * @brief 将 12 字节 STM32 UID 转换为 5 字节唯一 SN 码
 *
 * 算法: CRC32 低 4 字节 + XOR 校验字节
 * - 对 12 字节 UID 做 CRC32，取低 4 字节作为 SN[0..3]
 * - SN[4] = SN[0] ^ SN[1] ^ SN[2] ^ SN[3] (校验字节)
 *
 * @param uid 12 字节 UID 输入
 * @param sn  5 字节 SN 输出
 */
static void uid_to_sn(const uint8_t *uid, uint8_t *sn)
{
	uint32_t crc = 0xFFFFFFFF;

	/* CRC32 计算 */
	for (int i = 0; i < 12; i++) {
		crc ^= uid[i];
		for (int j = 0; j < 8; j++) {
			crc = (crc >> 1) ^ (0xEDB88320 & -(crc & 1));
		}
	}

	/* 取低 4 字节 */
	sn[0] = crc & 0xFF;
	sn[1] = (crc >> 8) & 0xFF;
	sn[2] = (crc >> 16) & 0xFF;
	sn[3] = (crc >> 24) & 0xFF;

	/* 校验字节 */
	sn[4] = sn[0] ^ sn[1] ^ sn[2] ^ sn[3];
}

#define USER_NODE DT_PATH(zephyr_user)
static const struct gpio_dt_spec charge_full = GPIO_DT_SPEC_GET(USER_NODE, chargefull_gpios);
static const struct gpio_dt_spec charging = GPIO_DT_SPEC_GET(USER_NODE, charging_gpios);
static const struct gpio_dt_spec power_button = GPIO_DT_SPEC_GET(USER_NODE, power_gpios);
static const struct gpio_dt_spec handler_button = GPIO_DT_SPEC_GET(USER_NODE, handlebt_gpios);
static const struct gpio_dt_spec link_switch = GPIO_DT_SPEC_GET(USER_NODE, linksw_gpios);

static const struct gpio_dt_spec can_power_gpio = GPIO_DT_SPEC_GET(USER_NODE, canpower_gpios);
static const struct gpio_dt_spec rf24_power_gpio = GPIO_DT_SPEC_GET(USER_NODE, rf24power_gpios);
static const struct gpio_dt_spec dis_power_gpio = GPIO_DT_SPEC_GET(USER_NODE, dispower_gpios);
static const struct gpio_dt_spec handler_power_gpio = GPIO_DT_SPEC_GET(USER_NODE, handlerpower_gpios);

static struct gpio_callback power_button_cb_data;
static struct gpio_callback linksw_cb_data;

static struct k_work_delayable btn_display_work;
static struct k_work_delayable linksw_work;
static struct k_work_delayable sleep_work;

static void btn_display_work_handler(struct k_work *work)
{
	if (global_params.sleeping) {
		return;
	}
	global_params.h_button = gpio_pin_get_dt(&handler_button);
	LOG_INF("handler button: %d", global_params.h_button);
	last_activity_time = k_uptime_get_32();

	uint8_t buf[8];
	sys_put_be16((uint16_t)global_params.x_degree, &buf[0]);
	sys_put_be16((uint16_t)global_params.y_degree, &buf[2]);
	buf[4] = global_params.h_button;
	buf[5] = buf[6] = buf[7] = 0xff;
	if (global_params.log) {
		LOG_INF("x: %d, y: %d, button: %d",
			global_params.x_degree, global_params.y_degree, global_params.h_button);
	}
	send_handler_state(buf, 8);
}

void connect_switch(uint8_t type)
{
	if (global_params.connect_type == CAN_TYPE) {
		k_event_clear(&global_params.event, RF24_EVENT);
		rf24_deinit();
		can_power_enable(true);
		k_event_post(&global_params.event, CAN_EVENT);
		mod_display_can();
	} else {
		k_event_clear(&global_params.event, CAN_EVENT);
		can_power_enable(false);
		rf24_init();
		k_event_post(&global_params.event, RF24_EVENT);
		mod_display_rf24(global_params.rssi);
	}
	LOG_INF("Link switch: %s", global_params.connect_type == CAN_TYPE ? "CAN" : "2.4G");
	persist_save_connect_type();
}

static void linksw_work_handler(struct k_work *work)
{
	if (gpio_pin_get_dt(&link_switch) == 0) {
		global_params.connect_type = (global_params.connect_type == CAN_TYPE) ? RF24_TYPE : CAN_TYPE;
		connect_switch(global_params.connect_type);
	}
}

int handler_get_btn(void)
{
	return gpio_pin_get_dt(&handler_button);
}

void can_power_enable(bool up)
{
	gpio_pin_set_dt(&can_power_gpio, up);
}

void rf24_power_enable(bool up)
{
	gpio_pin_set_dt(&rf24_power_gpio, up);
}

void dis_power_enable(bool up)
{
	gpio_pin_set_dt(&dis_power_gpio, up);
}

void handler_power_enable(bool up)
{
	gpio_pin_set_dt(&handler_power_gpio, up);
}

void system_sleep(void)
{
	k_event_clear(&global_params.event, WAKE_EVENT);
	global_params.sleeping = true;

	/* 等待各线程检测到 sleeping 标志并停止外设活动, 避免断电时 DMA 中断 */
	k_msleep(100);

	if (global_params.connect_type == CAN_TYPE) {
		can_power_enable(false);
	} else {
		rf24_deinit();
	}
	dis_power_enable(false);
	handler_power_enable(false);
}

static void system_wake(void)
{
	handler_power_enable(true);
	dis_power_enable(true);
	if (global_params.connect_type == CAN_TYPE) {
		can_power_enable(true);
	} else {
		rf24_init();
	}
	k_msleep(200);
	mod_display_reinit();
	mod_display_all(&global_params);
	global_params.sleeping = false;
	last_activity_time = k_uptime_get_32();
	k_event_post(&global_params.event, WAKE_EVENT);
	LOG_INF("system woke up");
}

static void sleep_work_handler(struct k_work *work)
{
	if (gpio_pin_get_dt(&power_button) == 0) {
		if (global_params.sleeping) {
			system_wake();
		}
	}
}

battery_status_t read_battery_status(void)
{
	int charge_full_pin = gpio_pin_get_dt(&charge_full);
	int charging_pin = gpio_pin_get_dt(&charging);

	if (charge_full_pin == 0) {
		return BATTERY_STATUS_FULL;
	}

	if (charging_pin == 0) {
		return BATTERY_STATUS_CHARGING;
	}

	return BATTERY_STATUS_DISCHARGING;
}

void gpio_irq(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	if (pins & BIT(power_button.pin)) {
		k_work_reschedule(&sleep_work, K_MSEC(10));
	} else if (pins & BIT(handler_button.pin)) {
		if (global_params.sleeping) {
			return;
		}
		k_work_reschedule(&btn_display_work, K_MSEC(10));
	}
}

static void linksw_irq(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	if (global_params.sleeping) {
		return;
	}
	k_work_reschedule(&linksw_work, K_MSEC(20));
}

static int power_init(void)
{
	int ret = 0;

	ret = gpio_pin_configure_dt(&can_power_gpio, GPIO_OUTPUT);
	if (ret < 0) {
		LOG_ERR("Failed to configure can power pin: %d", ret);
		return ret;
	}

	ret = gpio_pin_configure_dt(&rf24_power_gpio, GPIO_OUTPUT);
	if (ret < 0) {
		LOG_ERR("Failed to configure rf24 power pin: %d", ret);
		return ret;
	}

	ret = gpio_pin_configure_dt(&dis_power_gpio, GPIO_OUTPUT);
	if (ret < 0) {
		LOG_ERR("Failed to configure display power pin: %d", ret);
		return ret;
	}

	ret = gpio_pin_configure_dt(&handler_power_gpio, GPIO_OUTPUT);
	if (ret < 0) {
		LOG_ERR("Failed to configure 5v power pin: %d", ret);
		return ret;
	}

	global_params.report_period = REPORT_PERIOD_MS;
	global_params.connect_type = CAN_TYPE;
	global_params.log = false;

	/* 读取芯片唯一码，转换为 5 字节地址 (SN) */
	const uint8_t *uid = (const uint8_t *)UID_BASE;

	uid_to_sn(uid, global_params.rf24_addr);
	LOG_INF("SN: %02x%02x%02x%02x%02x",
		global_params.rf24_addr[0], global_params.rf24_addr[1],
		global_params.rf24_addr[2], global_params.rf24_addr[3],
		global_params.rf24_addr[4]);
	global_params.rf24_channel = 76; /* nRF24 默认信道 */

	k_event_init(&global_params.event);
	k_event_post(&global_params.event, CAN_EVENT);

	dis_power_enable(true);
	can_power_enable(true);
	handler_power_enable(true);

	return 0;
}

int gpio_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&charge_full)) {
		LOG_ERR("Charge full GPIO device not ready");
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&charging)) {
		LOG_ERR("Charging GPIO device not ready");
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&power_button)) {
		LOG_ERR("Power button GPIO device not ready");
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&handler_button)) {
		LOG_ERR("handle button GPIO device not ready");
		return -ENODEV;
	}

	if (!gpio_is_ready_dt(&link_switch)) {
		LOG_ERR("link switch GPIO device not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&charge_full, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("Failed to configure charge_full pin: %d", ret);
		return ret;
	}

	ret = gpio_pin_configure_dt(&charging, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("Failed to configure charging pin: %d", ret);
		return ret;
	}

	ret = gpio_pin_configure_dt(&power_button, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("Failed to configure power button pin: %d", ret);
		return ret;
	}

	ret = gpio_pin_configure_dt(&handler_button, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("Failed to configure handle button pin: %d", ret);
		return ret;
	}

	ret = gpio_pin_configure_dt(&link_switch, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("Failed to configure link switch pin: %d", ret);
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(&power_button, GPIO_INT_EDGE_FALLING);
	if (ret < 0) {
		LOG_ERR("Failed to configure power button interrupt: %d", ret);
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(&handler_button, GPIO_INT_EDGE_BOTH);
	if (ret < 0) {
		LOG_ERR("Failed to configure power button interrupt: %d", ret);
		return ret;
	}
	gpio_init_callback(&power_button_cb_data, gpio_irq,
				   BIT(power_button.pin) | BIT(handler_button.pin));
	gpio_add_callback(power_button.port, &power_button_cb_data);

	ret = gpio_pin_interrupt_configure_dt(&link_switch, GPIO_INT_EDGE_FALLING);
	if (ret < 0) {
		LOG_ERR("Failed to configure link switch interrupt: %d", ret);
		return ret;
	}
	gpio_init_callback(&linksw_cb_data, linksw_irq, BIT(link_switch.pin));
	gpio_add_callback(link_switch.port, &linksw_cb_data);

	k_work_init_delayable(&btn_display_work, btn_display_work_handler);
	k_work_init_delayable(&linksw_work, linksw_work_handler);
	k_work_init_delayable(&sleep_work, sleep_work_handler);
	global_params.h_button = gpio_pin_get_dt(&handler_button);

	LOG_INF("GPIO initialized successfully");
	LOG_INF("  PA3 (Charge Full): %s", gpio_pin_get_dt(&charge_full) ? "HIGH" : "LOW");
	LOG_INF("  PA2 (Charging):    %s", gpio_pin_get_dt(&charging) ? "HIGH" : "LOW");
	LOG_INF("  PA1 (Power Button): %s", gpio_pin_get_dt(&power_button) ? "HIGH" : "LOW");

	return 0;
}

SYS_INIT(power_init, PRE_KERNEL_2, 1);

#ifdef CONFIG_SHELL
#include <zephyr/shell/shell.h>
#include <rf24.h>
#include <nrf24l01p.h>

static int cmd_link_log(const struct shell *ctx, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(ctx, "log: %s", global_params.log ? "on" : "off");
		return 0;
	}

	int val = (int)strtol(argv[1], NULL, 10);

	global_params.log = (val != 0);
	shell_print(ctx, "log: %s", global_params.log ? "on" : "off");
	return 0;
}

static int cmd_link_can(const struct shell *ctx, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	global_params.connect_type = CAN_TYPE;
	connect_switch(global_params.connect_type);
	return 0;
}

static int cmd_link_rf24(const struct shell *ctx, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	global_params.connect_type = RF24_TYPE;
	connect_switch(global_params.connect_type);
	return 0;
}

/* 测试当前链路通信：发 count 次 TEST_FRAME，统计成功/失败 */
static int cmd_link_test(const struct shell *ctx, size_t argc, char **argv)
{
	int count = 10;

	if (argc > 1) {
		count = (int)strtol(argv[1], NULL, 10);
		if (count < 1) {
			count = 1;
		} else if (count > 100) {
			count = 100;
		}
	}

	int ok = 0, failed = 0;

	for (int i = 0; i < count; i++) {
		uint8_t seq = (uint8_t)i;
		bool success;

		if (global_params.connect_type == CAN_TYPE) {
			struct can_frame frame = {
				.id = TEST_FRAME,
				.dlc = can_bytes_to_dlc(1),
			};
			frame.data[0] = seq;
			success = mod_can_send(&frame) >= 0;
		} else {
			success = rf24_data_send(TEST_FRAME, &seq, 1);
		}
		if (success) {
			ok++;
		} else {
			failed++;
		}
	}

	shell_print(ctx, "test: %d sent, %d ok, %d failed", count, ok, failed);
	return 0;
}

/* 设置 nRF24 信道: link rf24_ch <0-125> */
static int cmd_link_rf24_ch(const struct shell *ctx, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_print(ctx, "rf24 channel: %d", global_params.rf24_channel);
		return 0;
	}

	int ch = (int)strtol(argv[1], NULL, 10);

	if (ch < 0 || ch > RF24_ADDR_MAX_CH) {
		shell_error(ctx, "invalid channel: %d (0-%d)", ch, RF24_ADDR_MAX_CH);
		return -EINVAL;
	}

	global_params.rf24_channel = (uint8_t)ch;
	shell_print(ctx, "rf24 channel set to: %d", global_params.rf24_channel);
	return 0;
}

/* 查询当前 rf24 配置: link rf24_get */
static int cmd_link_rf24_get(const struct shell *ctx, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	char addr_str[RF24_ADDR_LEN * 2 + 1] = {0};

	for (int i = 0; i < RF24_ADDR_LEN; i++) {
		snprintf(addr_str + i * 2, 3, "%02x", global_params.rf24_addr[i]);
	}
	shell_print(ctx, "rf24 channel: %d", global_params.rf24_channel);
	shell_print(ctx, "rf24 addr:   %s", addr_str);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_link_cmds,
	SHELL_CMD_ARG(can, NULL, "Switch to CAN", cmd_link_can, 1, 0),
	SHELL_CMD_ARG(rf24, NULL, "Switch to 2.4G (nRF24L01+)", cmd_link_rf24, 1, 0),
	SHELL_CMD_ARG(test, NULL, "Test current link [count]", cmd_link_test, 1, 1),
	SHELL_CMD_ARG(log, NULL, "Enable/disable debug log [0/1]", cmd_link_log, 1, 1),
	SHELL_CMD_ARG(rf24_ch, NULL, "Get/set nRF24 channel [0-125]", cmd_link_rf24_ch, 1, 1),
	SHELL_CMD_ARG(rf24_get, NULL, "Query current nRF24 config", cmd_link_rf24_get, 1, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(link, &sub_link_cmds, "Link switch commands", NULL);
#endif
