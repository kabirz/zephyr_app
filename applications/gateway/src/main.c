/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Gateway 主入口 - 数据中转网关
 * 接收 mod_handler 的 nRF24 数据，通过 CAN 或 UDP 转发给上位机
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/settings/settings.h>
#include <gateway.h>
#include <zephyr/app_version.h>
#ifdef CONFIG_GW_NETWORKING
#include <zephyr/net/net_if.h>
#include <zephyr/net/socket.h>
#endif
#ifndef CONFIG_FLASH_SIZE
#define CONFIG_FLASH_SIZE 0x1000
#endif

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(gateway, LOG_LEVEL_INF);

gateway_params_t gw_params;

#define USER_NODE DT_PATH(zephyr_user)

/* ================================================================
 * 外设电源使能 (手柄板: CAN/nRF24/5V 主电源 GPIO 独立控制)
 * Zephyr 设备驱动初始化前上电, 确保 nRF24/CAN 芯片就绪
 * ================================================================ */
static const struct gpio_dt_spec can_power = GPIO_DT_SPEC_GET(USER_NODE, canpower_gpios);
static const struct gpio_dt_spec main_power = GPIO_DT_SPEC_GET(USER_NODE, mainpower_gpios);

static int gw_power_init(void)
{
	if (!gpio_is_ready_dt(&can_power) || !gpio_is_ready_dt(&main_power)) {
		LOG_ERR("Power GPIO not ready");
		return -ENODEV;
	}

	gpio_pin_configure_dt(&can_power, GPIO_OUTPUT);
	gpio_pin_configure_dt(&main_power, GPIO_OUTPUT);

	gpio_pin_set_dt(&main_power, 1);  /* 系统主电源 (5V) */
	gpio_pin_set_dt(&can_power, 1);   /* CAN 收发器 */

	/* nRF24 模块电源 (PC9) 由驱动按 power-gpios 管理: init 时上电 + POR 延时 */
	LOG_INF("Power rails enabled (CAN/5V)");
	return 0;
}
SYS_INIT(gw_power_init, PRE_KERNEL_2, 1);

/* ================================================================
 * Link Switch GPIO - 切换 CAN/UDP 模式 (需 CONFIG_GW_NETWORKING)
 * ================================================================ */
static const struct gpio_dt_spec link_switch = GPIO_DT_SPEC_GET(USER_NODE, linksw_gpios);
static struct gpio_callback linksw_cb_data;
static struct k_work_delayable linksw_work;

static void linksw_work_handler(struct k_work *work)
{
	if (gpio_pin_get_dt(&link_switch) == 0) {
#ifdef CONFIG_GW_NETWORKING
		gw_params.connect_type = (gw_params.connect_type == GW_MODE_CAN) ? GW_MODE_UDP
									       : GW_MODE_CAN;
		LOG_INF("Link mode: %s", gw_params.connect_type == GW_MODE_CAN ? "CAN" : "UDP");
		persist_save_network_config();
#else
		LOG_INF("Link switch ignored (networking disabled, CAN-only)");
#endif
	}
}

static void linksw_irq(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
	k_work_reschedule(&linksw_work, K_MSEC(20));
}

static int linksw_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&link_switch)) {
		LOG_ERR("Link switch GPIO not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&link_switch, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("Failed to configure link switch pin: %d", ret);
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(&link_switch, GPIO_INT_EDGE_FALLING);
	if (ret < 0) {
		LOG_ERR("Failed to configure link switch interrupt: %d", ret);
		return ret;
	}

	gpio_init_callback(&linksw_cb_data, linksw_irq, BIT(link_switch.pin));
	gpio_add_callback(link_switch.port, &linksw_cb_data);
	k_work_init_delayable(&linksw_work, linksw_work_handler);

	LOG_INF("Link switch initialized");
	return 0;
}

#ifdef CONFIG_GW_NETWORKING
/* ================================================================
 * 网络初始化 (W5500 静态 IP)
 * ================================================================ */
static int net_init(void)
{
	struct net_if *iface = net_if_get_default();
	struct in_addr addr, mask, gw;

	if (!iface) {
		LOG_ERR("No network interface found");
		return -ENODEV;
	}

	if (net_addr_pton(AF_INET, gw_params.ip_addr, &addr) < 0) {
		LOG_ERR("Invalid IP address: %s", gw_params.ip_addr);
		return -EINVAL;
	}
	if (net_addr_pton(AF_INET, gw_params.netmask, &mask) < 0) {
		LOG_ERR("Invalid netmask: %s", gw_params.netmask);
		return -EINVAL;
	}
	if (net_addr_pton(AF_INET, gw_params.gateway, &gw) < 0) {
		LOG_ERR("Invalid gateway: %s", gw_params.gateway);
		return -EINVAL;
	}

	net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0);
	net_if_ipv4_set_netmask_by_addr(iface, &addr, &mask);
	net_if_ipv4_set_gw(iface, &gw);

	LOG_INF("Network: %s/%s gw %s", gw_params.ip_addr, gw_params.netmask, gw_params.gateway);
	return 0;
}
#endif

int main(void)
{
	LOG_INF("build time: %s-%s", __DATE__, __TIME__);
	LOG_INF("board: %s, system clk: %dMHz", CONFIG_BOARD_TARGET,
		CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC / MHZ(1));
	LOG_INF("flash size: %dKB, ram size: %dKB", CONFIG_FLASH_SIZE, CONFIG_SRAM_SIZE);
	LOG_INF("version: %s", APP_VERSION_STRING);
	LOG_INF("networking: %s", IS_ENABLED(CONFIG_GW_NETWORKING) ? "enabled" : "disabled");

	/* 初始化默认配置 */
	gw_params.connect_type = GW_MODE_CAN;
	gw_params.rf24_channel = RF24_DEFAULT_CH;
	gw_params.rf24_addr[0] = 0;
	gw_params.rf24_addr[1] = 0;
	gw_params.rf24_addr[2] = 0;
	gw_params.rf24_addr[3] = 0;
	gw_params.rf24_addr[4] = 0;
#ifdef CONFIG_GW_NETWORKING
	strncpy(gw_params.ip_addr, GATEWAY_DEFAULT_IP, sizeof(gw_params.ip_addr) - 1);
	strncpy(gw_params.netmask, GATEWAY_DEFAULT_MASK, sizeof(gw_params.netmask) - 1);
	strncpy(gw_params.gateway, GATEWAY_DEFAULT_GW, sizeof(gw_params.gateway) - 1);
	gw_params.udp_port = GATEWAY_DEFAULT_UDP_PORT;
#endif


	/* 加载持久化配置 (覆盖默认值) */
	gw_config_load();

	/* 初始化链路切换按键 */
	linksw_init();


	/* 初始化各模块 */
	gw_rf24_init();

#ifdef CONFIG_GW_NETWORKING
	/* 等待网络就绪后初始化 */
	k_msleep(500);
	net_init();
#endif

	gw_params.running = true;
	LOG_INF("Gateway ready");

	while (1) {
		k_sleep(K_MSEC(1000));
	}

	return 0;
}

/* ================================================================
 * Shell 命令: link can / link udp
 * ================================================================ */
#ifdef CONFIG_SHELL
#include <zephyr/shell/shell.h>

static int cmd_link_can(const struct shell *ctx, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	gw_params.connect_type = GW_MODE_CAN;
	LOG_INF("Link mode: CAN");
	shell_print(ctx, "mode: CAN");
	return 0;
}

#ifdef CONFIG_GW_NETWORKING
static int cmd_link_udp(const struct shell *ctx, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	gw_params.connect_type = GW_MODE_UDP;
	LOG_INF("Link mode: UDP");
	shell_print(ctx, "mode: UDP");
	return 0;
}
#endif

static int cmd_link_status(const struct shell *ctx, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	shell_print(ctx, "mode: %s", gw_params.connect_type == GW_MODE_CAN ? "CAN" : "UDP");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_link_cmds,
	SHELL_CMD_ARG(can, NULL, "Switch to CAN mode", cmd_link_can, 1, 0),
#ifdef CONFIG_GW_NETWORKING
	SHELL_CMD_ARG(udp, NULL, "Switch to UDP mode", cmd_link_udp, 1, 0),
#endif
	SHELL_CMD_ARG(status, NULL, "Show current mode", cmd_link_status, 1, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(link, &sub_link_cmds, "Link mode switch commands", NULL);
#endif
