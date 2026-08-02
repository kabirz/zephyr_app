/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * 三路状态灯管理
 *   PA1 (led_rf24): 2.4G 状态灯 - rf24 初始化完成后常亮, 收发时闪烁
 *   PA2 (led_err):  错误灯     - 栈溢出/hardfault/关键硬件初始化失败点亮, 锁定不灭
 *   PA3 (led_sys):  系统灯     - 进入 main 循环前点亮
 *
 * 全部低电平亮 (GPIO_ACTIVE_LOW), 通过 DT 描述; 代码用 gpio_pin_set_dt 的逻辑电平
 * (1=亮, 0=灭), 不关心物理极性.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <gateway.h>

LOG_MODULE_REGISTER(gw_led, LOG_LEVEL_INF);

/* DT alias 引用, 编译期校验引脚存在 */
static const struct gpio_dt_spec led_rf24 = GPIO_DT_SPEC_GET(DT_ALIAS(led_rf24), gpios);
static const struct gpio_dt_spec led_err  = GPIO_DT_SPEC_GET(DT_ALIAS(led_err), gpios);
static const struct gpio_dt_spec led_sys  = GPIO_DT_SPEC_GET(DT_ALIAS(led_sys), gpios);

/* 2.4G 收发活动时间戳 (由收发路径无锁更新, LED 线程读取).
 * 32-bit ms 回绕周期 ~49 天, 单调足够; 读写在 32-bit ARM 上原子. */
static volatile uint32_t rf24_last_activity;
/* 错误灯锁定标志 - 一旦置位, LED 线程不再干预 PA2 */
static volatile bool error_latched;

/* 活动闪烁窗口: 最后一次收发后此时间内灯灭, 之后恢复常亮.
 * 80ms 让肉眼能感知单帧引发的短暂熄灭, 又不会在连续流量时频繁抖动. */
#define RF24_ACTIVITY_WINDOW_MS  80
/* LED 线程扫描周期: 小于闪烁窗口以保证熄灭期不被漏判 */
#define LED_SCAN_PERIOD_MS       30

/* ================================================================
 * 初始化: PA1/PA2/PA3 配置为输出, 默认全灭; 随后点亮 PA1
 * (rf24 驱动初始化紧随其后, 此处点灯即"2.4G 模块就绪"指示).
 * ================================================================ */
void gw_led_init(void)
{
	gpio_pin_configure_dt(&led_rf24, GPIO_OUTPUT_INACTIVE);  /* 初始灭 */
	gpio_pin_configure_dt(&led_err,  GPIO_OUTPUT_INACTIVE);
	gpio_pin_configure_dt(&led_sys,  GPIO_OUTPUT_INACTIVE);

	gpio_pin_set_dt(&led_rf24, 1);   /* 2.4G 就绪常亮 */
	gpio_pin_set_dt(&led_err,  0);
	gpio_pin_set_dt(&led_sys,  0);
}

void gw_led_sys_on(void)
{
	gpio_pin_set_dt(&led_sys, 1);
}

void gw_led_error_on(void)
{
	error_latched = true;
	gpio_pin_set_dt(&led_err, 1);
}

void gw_led_rf24_activity(void)
{
	/* 仅一次赋值, 收发热路径零阻塞, 不取锁 */
	rf24_last_activity = k_uptime_get_32();
}

/* ================================================================
 * LED 扫描线程 (最低优先级, 不抢占任何业务线程)
 *   - 错误灯锁定后保持亮 (不灭)
 *   - PA1 在活动窗口内灭, 窗口外常亮 (高频收发持续灭, 间歇收发闪烁)
 * ================================================================ */
static void led_thread(void)
{
	uint32_t last_rf24_state = 1;  /* 初始常亮, 避免启动时多余翻转 */

	while (1) {
		uint32_t now = k_uptime_get_32();
		uint32_t last = rf24_last_activity;
		/* 处理 32-bit 回绕: (now - last) 在无符号减法下天然正确 */
		uint32_t elapsed = now - last;
		uint32_t target = (elapsed < RF24_ACTIVITY_WINDOW_MS) ? 0 : 1;

		/* 仅在状态变化时写 GPIO, 减少 bus 访问 */
		if (target != last_rf24_state) {
			gpio_pin_set_dt(&led_rf24, target);
			last_rf24_state = target;
		}

		k_msleep(LED_SCAN_PERIOD_MS);
	}
}

K_THREAD_DEFINE(thread_led, CONFIG_GATEWAY_LED_STACK, led_thread, NULL, NULL, NULL,
		CONFIG_GATEWAY_LED_PRIORITY, 0, 0);

/* ================================================================
 * Zephyr fatal error handler - 覆盖默认弱符号
 * 栈溢出 / CPU 异常 (hardfault 等) 时点亮错误灯锁定, 之后交还默认处理流程
 * (不做 reboot, 让系统进入 fault dump / LOG_PANIC 以便调试).
 * ================================================================ */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	ARG_UNUSED(esf);

	if (reason == K_ERR_STACK_CHK_FAIL || reason == K_ERR_CPU_EXCEPTION) {
		/* fatal 上下文 (栈已损/ISR 中) 不调 LOG, 仅点亮错误灯这一可靠副作用.
		 * 之后交还默认 fatal 处理流程做 dump/panic. */
		gw_led_error_on();
	}
}
