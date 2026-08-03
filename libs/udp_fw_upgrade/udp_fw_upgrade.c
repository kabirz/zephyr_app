/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * UDP 固件升级库 - 自包含实现 (参考 can_fw_upgrade)
 *
 * 库通过 SYS_INIT 自动创建配置端口 UDP socket (INADDR_ANY:9200),
 * 自管 RX 线程, 内部处理固件升级命令 (FW_START/DATA/END);
 * 其他配置命令通过应用注册的回调分发.
 */

#include <string.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/app_version.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_core.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/posix/unistd.h>
#include <zephyr/posix/arpa/inet.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/reboot.h>
#include "udp_fw_upgrade.h"

#ifdef CONFIG_MCUBOOT_SIGNATURE_KEY_FILE
#include <fw_keyhash.h>
#endif

LOG_MODULE_REGISTER(udp_fw_upgrade, LOG_LEVEL_INF);

/* 库内命令码 (配置端口帧首字节, 从 0 开始连续编号, 对齐 can_fw_upgrade).
 * 应用业务命令从 0x10 起, 不会与此区间冲突. */
enum fw_cmd {
	FW_CMD_START       = 1,   /* 开始升级 (擦 slot1 + init) */
	FW_CMD_DATA        = 2,   /* 固件数据写入 */
	FW_CMD_END         = 3,   /* 结束升级 (flush + CRC 校验 + boot_request_upgrade) */
	FW_CMD_GET_VERSION = 4,   /* 查询固件版本 */
	FW_CMD_REBOOT      = 5,   /* 重启设备 */
};

#define SLOT1_PARTITION_ID PARTITION_ID(slot1_partition)

/* FW_START 应答状态字节 (保持旧值时兼容既有上位机) */
enum fw_start_status {
	FW_START_ERR_FAIL     = 0,  /* 启动失败 (擦除/初始化) 或未开始 */
	FW_START_OK           = 1,  /* 校验通过, 已开始升级 (原有语义) */
	FW_START_ERR_KEYHASH  = 2,  /* keyhash 不一致, 已拒绝 */
};

/* ================================================================
 * 全局状态
 * ================================================================ */

/* 应用命令回调 */
static udp_fw_app_cmd_cb_t app_handler;
static void *app_user_data;

/* 配置端口 socket + 发送方地址 (回复路由用) */
static int config_sock = -1;
static struct sockaddr_in config_remote_addr;

/* 固件升级状态 */
static struct flash_img_context flash_img_ctx;
static bool fw_started;
static uint32_t fw_size;        /* START 保存的固件大小 */
static uint32_t fw_received;    /* 已接收字节数 (用于 DATA offset 回复) */

#define FW_CRC_CHUNK 64         /* 读回 slot1 重算 CRC 的分块大小 */

/* ================================================================
 * 子网判断 + 回复
 * ================================================================ */

/* 判断发送方 IP 是否与本机同子网.
 * 本机 IP 从网络接口直接获取 (net_if_ipv4_get_global_addr),
 * 不依赖应用参数结构体, 保证库的自治性. */
static bool is_same_subnet(struct in_addr sender_ip)
{
	struct net_if *iface = net_if_get_default();

	if (!iface) {
		return true;  /* 无法判断时按同子网处理 (单播) */
	}

	/* 从接口获取本机首选全局 IPv4 地址 */
	struct in_addr *local_p = (struct in_addr *)
		net_if_ipv4_get_global_addr(iface, NET_ADDR_PREFERRED);

	if (!local_p) {
		return true;  /* 还没拿到 IP, 按同子网 (单播) */
	}

	/* 用本机 IP 反查 netmask (与应用侧 is_same_subnet 逻辑一致) */
	struct net_in_addr nm = net_if_ipv4_get_netmask_by_addr(
		iface, (const struct net_in_addr *)local_p);
	struct in_addr mask = *(struct in_addr *)&nm;

	return (sender_ip.s_addr & mask.s_addr) ==
	       (local_p->s_addr & mask.s_addr);
}

/* 配置端口回复: 通过配置 socket 回复. 同子网单播给发送方, 跨子网广播.
 * 回复格式: [cmd 1B][data...] (无魔数头) */
void udp_fw_reply(uint8_t cmd, const uint8_t *data, uint8_t len)
{
	if (config_sock < 0) {
		return;
	}

	uint8_t buf[64] = { 0 };
	size_t send_len = 1;  /* cmd */

	buf[0] = cmd;
	if (len > 0 && len <= sizeof(buf) - 1) {
		memcpy(buf + 1, data, len);
		send_len += len;
	}

	struct sockaddr_in dst;

	if (is_same_subnet(config_remote_addr.sin_addr)) {
		/* 同子网: 单播回复到发送方源地址 (端口=发送方源端口) */
		dst = config_remote_addr;
	} else {
		/* 跨子网: 广播回复. 远程端口 = 本地端口 + 1 (上位机监听 config+1) */
		dst.sin_family = AF_INET;
		dst.sin_port = htons(CONFIG_UDP_FW_CONFIG_PORT + 1);
		dst.sin_addr.s_addr = INADDR_BROADCAST;
	}

	sendto(config_sock, buf, send_len, 0, (struct sockaddr *)&dst, sizeof(dst));
}

/* ================================================================
 * 固件升级命令处理
 * ================================================================ */

/* 读回 slot1 已写数据, 重算 CRC16-CCITT, 与 recv_crc 比对 */
static bool fw_verify_crc(uint16_t recv_crc)
{
	const struct flash_area *fa;

	if (flash_area_open(SLOT1_PARTITION_ID, &fa) != 0) {
		LOG_ERR("CRC verify: flash_area_open failed");
		return false;
	}

	size_t written = flash_img_bytes_written(&flash_img_ctx);
	uint16_t calc_crc = 0;
	uint8_t buf[FW_CRC_CHUNK];

	for (size_t off = 0; off < written; off += FW_CRC_CHUNK) {
		size_t len = (written - off < FW_CRC_CHUNK) ? (written - off) : FW_CRC_CHUNK;

		if (flash_area_read(fa, off, buf, len) != 0) {
			flash_area_close(fa);
			LOG_ERR("CRC verify: flash_area_read failed @%zu", off);
			return false;
		}
		calc_crc = crc16_ccitt(calc_crc, buf, len);
	}

	flash_area_close(fa);
	if (calc_crc != recv_crc) {
		LOG_ERR("CRC mismatch: calc=0x%04x recv=0x%04x", calc_crc, recv_crc);
		return false;
	}
	return true;
}

/* 处理固件升级命令. 返回 true 表示是固件命令 (已处理). */
static bool handle_fw_cmd(uint8_t cmd, const uint8_t *data, size_t len)
{
	switch (cmd) {
case FW_CMD_START: {
		uint8_t status = FW_START_ERR_FAIL;

		if (!fw_started && len >= 4) {
			fw_size = sys_get_le32(data);

			/* 升级前 keyhash 校验 (CONFIG_MCUBOOT_SIGNATURE_KEY_FILE): 仅当上位机在
			 * FW_START 携带 [keyhash 32B] (len==4+32) 时才校验; 老上位机发
			 * 旧 4B 帧 (无 keyhash) 时放行, 兼容旧协议. 不一致则拒绝且不触 flash. */
#ifdef CONFIG_MCUBOOT_SIGNATURE_KEY_FILE
			if (len == 4 + FW_KEYHASH_KEY_LEN &&
			    memcmp(data + 4, fw_keyhash, FW_KEYHASH_KEY_LEN) != 0) {
				LOG_WRN("FW_START rejected: keyhash mismatch");
				status = FW_START_ERR_KEYHASH;
				udp_fw_reply(cmd, &status, 1);
				return true;
			}
#endif

			const struct flash_area *fa;

			if (flash_area_open(SLOT1_PARTITION_ID, &fa) != 0) {
				LOG_ERR("FW_START: flash_area_open failed");
			} else {
				flash_area_erase(fa, 0, fa->fa_size);
				flash_area_close(fa);
				if (flash_img_init(&flash_img_ctx) != 0) {
					LOG_ERR("FW_START: flash_img_init failed");
				} else {
					fw_started = true;
					fw_received = 0;
					status = FW_START_OK;
					LOG_INF("FW upgrade started, size=%u", fw_size);
				}
			}
		}
		udp_fw_reply(cmd, &status, 1);
		return true;
	}

	case FW_CMD_DATA: {
		uint8_t off[4] = { 0 };

		if (fw_started && len > 0) {
			if (flash_img_buffered_write(&flash_img_ctx, data, len, false) == 0) {
				fw_received += len;
				sys_put_le32(fw_received, off);
			} else {
				LOG_ERR("FW_DATA: flash write failed");
				fw_started = false;
			}
		}
		udp_fw_reply(cmd, off, 4);
		return true;
	}

	case FW_CMD_END: {
		uint8_t result = 0;

		if (fw_started && len >= 3) {
			uint8_t test_mode = data[0];
			uint16_t recv_crc = sys_get_le16(data + 1);

			flash_img_buffered_write(&flash_img_ctx, NULL, 0, true);

			if (fw_verify_crc(recv_crc)) {
				int ret = boot_request_upgrade(test_mode ? 0 : 1);

				if (ret == 0) {
					result = 1;
					LOG_INF("FW upgrade verified (test_mode=%d), waiting for reboot",
						test_mode);
				} else {
					LOG_ERR("FW_END: boot_request_upgrade failed: %d", ret);
				}
			} else {
				LOG_ERR("FW_END: CRC mismatch, upgrade rejected");
			}
			fw_started = false;
		}
		udp_fw_reply(cmd, &result, 1);
		return true;
	}

	case FW_CMD_GET_VERSION:
		/* 响应 APP_VERSION_STRING (含 EXTRAVERSION, 如 "0.1.0-dev").
		 * 变长字符串, 不含末尾 '\0' */
		udp_fw_reply(cmd, (const uint8_t *)APP_VERSION_STRING,
			     strlen(APP_VERSION_STRING));
		return true;

	case FW_CMD_REBOOT:
		LOG_INF("reboot requested");
		udp_fw_reply(cmd, NULL, 0);
		k_msleep(100);
		sys_reboot(SYS_REBOOT_COLD);
		return true;

	default:
		return false;
	}
}

/* ================================================================
 * RX 线程 (静态): 收配置命令 → 固件命令内部处理, 其余分发给应用回调
 * ================================================================ */
static void udp_fw_rx_thread(void *p1, void *p2, void *p3)
{
	static uint8_t buf[512];

	while (1) {
		struct sockaddr_in src_addr;
		socklen_t addr_len = sizeof(src_addr);

		ssize_t received = recvfrom(config_sock, buf, sizeof(buf), 0,
					    (struct sockaddr *)&src_addr, &addr_len);
		if (received <= 0) {
			continue;
		}

		/* 记录配置端口发送方地址 (回复路由用) */
		config_remote_addr = src_addr;

		uint8_t cmd = buf[0];
		const uint8_t *cmd_data = buf + 1;
		size_t cmd_len = received - 1;

		/* 固件升级命令: 内部处理 */
		if (handle_fw_cmd(cmd, cmd_data, cmd_len)) {
			continue;
		}

		/* 其他命令: 分发给应用回调 */
		if (app_handler) {
			if (!app_handler(cmd, cmd_data, cmd_len, app_user_data)) {
				LOG_WRN("unhandled UDP cmd: 0x%02x", cmd);
			}
		} else {
			LOG_WRN("no app handler for UDP cmd: 0x%02x", cmd);
		}
	}
}

K_THREAD_DEFINE(udp_fw_rx_thread_id, CONFIG_UDP_FW_RX_STACK_SIZE,
		udp_fw_rx_thread, NULL, NULL, NULL,
		CONFIG_UDP_FW_RX_PRIORITY, 0, SYS_FOREVER_MS);

/* ================================================================
 * 网络就绪后启动: 创建配置端口 socket + bind + 启动 RX 线程
 * 通过 net_mgmt NET_EVENT_IF_UP 等待接口 up, 适配各应用网络初始化时序
 * (如 gateway_udp_test 在 main 里手动 net_init, 接口注册晚).
 * ================================================================ */
static bool udp_fw_started;

static void udp_fw_start(void)
{
	if (udp_fw_started) {
		return;
	}

	config_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (config_sock < 0) {
		LOG_ERR("config socket create failed: %d", errno);
		return;
	}

	/* 允许广播收发 */
	int broadcast = 1;

	setsockopt(config_sock, SOL_SOCKET, SO_BROADCAST,
		   (const char *)&broadcast, sizeof(broadcast));

	/* 绑定 INADDR_ANY: 上位机可广播配置命令到此端口 */
	struct sockaddr_in local_addr = {
		.sin_family = AF_INET,
		.sin_port = htons(CONFIG_UDP_FW_CONFIG_PORT),
	};

	if (bind(config_sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
		LOG_ERR("config socket bind failed: %d", errno);
		close(config_sock);
		config_sock = -1;
		return;
	}

	udp_fw_started = true;
	LOG_INF("config port %d listening (fw upgrade ready)", CONFIG_UDP_FW_CONFIG_PORT);

	/* 启动 RX 线程 */
	k_thread_start(udp_fw_rx_thread_id);
}

/* NET_EVENT_IF_UP 回调: 接口 up 后启动配置端口 */
static struct net_mgmt_event_callback if_up_cb;

static void if_up_handler(struct net_mgmt_event_callback *cb,
			  uint64_t mgmt_event, struct net_if *iface)
{
	if (mgmt_event == NET_EVENT_IF_UP) {
		udp_fw_start();
	}
}

/* SYS_INIT: 检查接口是否已 up, 是则直接启动; 否则注册 net_mgmt 等 IF_UP */
static int udp_fw_init(void)
{
	struct net_if *iface = net_if_get_default();

	/* 接口已 up (网络在 SYS_INIT 前就绑定好): 直接启动 */
	if (iface && net_if_oper_state(iface) == NET_IF_OPER_UP) {
		udp_fw_start();
		return 0;
	}

	/* 接口未 up: 注册 net_mgmt 等待 IF_UP 事件 (适用于 main 里手动 net_init 的应用) */
	net_mgmt_init_event_callback(&if_up_cb, if_up_handler, NET_EVENT_IF_UP);
	net_mgmt_add_event_callback(&if_up_cb);
	LOG_INF("waiting for network interface up...");
	return 0;
}

SYS_INIT(udp_fw_init, APPLICATION, CONFIG_UDP_FW_INIT_PRIORITY);

/* ================================================================
 * 对外接口实现
 * ================================================================ */
void udp_fw_set_app_handler(udp_fw_app_cmd_cb_t cb, void *user_data)
{
	app_handler = cb;
	app_user_data = user_data;
}
