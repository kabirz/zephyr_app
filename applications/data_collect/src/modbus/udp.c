#include "init.h"
#include <zephyr/net/socket.h>
#include <zephyr/data/json.h>
#include <zephyr/net/net_if.h>
#ifdef CONFIG_SETTINGS
#include <zephyr/settings/settings.h>
#endif

#define MULTICAST_GROUP "224.0.0.1"
#define MULTICAST_PORT  9002

struct get_info {
	bool get_device_info;
};

struct device_info {
	uint32_t ip[4];
	size_t ip_len;
	uint32_t slave_id;
	uint32_t rs485_bps;
	uint32_t timestamp;
};

struct set_info {
	struct device_info set_device_info;
};

static const struct json_obj_descr get_info_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct get_info, get_device_info, JSON_TOK_TRUE),
};

static const struct json_obj_descr device_info_descr[] = {
	JSON_OBJ_DESCR_ARRAY(struct device_info, ip, 4, ip_len, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct device_info, slave_id, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct device_info, rs485_bps, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct device_info, timestamp, JSON_TOK_NUMBER),
};

static const struct json_obj_descr set_info_descr[] = {
	JSON_OBJ_DESCR_OBJECT(struct set_info, set_device_info, device_info_descr),
};
#define ERROR_MSG "{\"errorMsg\":\"parse json error\"}"
#define NULL_MSG  "{}"

static int get_d_info(uint8_t *buf, size_t len)
{
	struct device_info dev_info;

	dev_info.timestamp = (uint32_t)time(NULL);
	dev_info.slave_id = get_holding_reg(HOLDING_SLAVE_ID_IDX);
	dev_info.rs485_bps = get_holding_reg(HOLDING_RS485_BPS_IDX);
	for (int i = 0; i < ARRAY_SIZE(dev_info.ip); i++) {
		dev_info.ip[i] = (uint8_t)get_holding_reg(HOLDING_IP_ADDR_1_IDX + i);
	}
	dev_info.ip_len = 4;
	json_obj_encode_buf(device_info_descr, ARRAY_SIZE(device_info_descr), &dev_info, buf, len);
	return strlen(buf);
}

static int parse_udp_msg(uint8_t *msg, size_t len)
{
	int ret;
	struct set_info s_info;
	struct get_info g_info;

	ret = json_obj_parse(msg, strlen(msg), get_info_descr, ARRAY_SIZE(get_info_descr), &g_info);
	if (ret < 0) {
		LOG_ERR("parse error: %d", ret);
		memcpy(msg, ERROR_MSG, sizeof(ERROR_MSG));
		return sizeof(ERROR_MSG) - 1;
	}

	if (ret > 0 && g_info.get_device_info == true) {
		return get_d_info(msg, len);
	}

	ret = json_obj_parse(msg, strlen(msg), set_info_descr, ARRAY_SIZE(set_info_descr), &s_info);
	if (ret > 0) {
		bool reg_changed = false;

		if (s_info.set_device_info.ip_len == 4) {
			update_holding_reg(HOLDING_IP_ADDR_1_IDX, s_info.set_device_info.ip[0]);
			update_holding_reg(HOLDING_IP_ADDR_2_IDX, s_info.set_device_info.ip[1]);
			update_holding_reg(HOLDING_IP_ADDR_3_IDX, s_info.set_device_info.ip[2]);
			update_holding_reg(HOLDING_IP_ADDR_4_IDX, s_info.set_device_info.ip[3]);
			reg_changed = true;
		}
		if (s_info.set_device_info.slave_id) {
			update_holding_reg(HOLDING_SLAVE_ID_IDX,
					   (uint16_t)s_info.set_device_info.slave_id);
			reg_changed = true;
		}
		if (s_info.set_device_info.timestamp) {
			extern void set_timestamp(time_t);
			set_timestamp((time_t)s_info.set_device_info.timestamp);
		}
		if (s_info.set_device_info.rs485_bps) {
			update_holding_reg(HOLDING_RS485_BPS_IDX,
					   (uint16_t)s_info.set_device_info.rs485_bps);
			reg_changed = true;
		}
		if (reg_changed) {
#ifdef CONFIG_SETTINGS
			settings_save();
#endif
		}
		return get_d_info(msg, len);
	}
	memcpy(msg, NULL_MSG, sizeof(NULL_MSG));
	return sizeof(NULL_MSG) - 1;
}

static void udp_poll(void)
{
	int serv;
	struct sockaddr_in bind_addr, client_addr;
	socklen_t client_addr_len;
	int buf_len;
	uint8_t udp_buffer[256];

	while (!net_if_is_admin_up(net_if_get_default())) {
		k_msleep(200);
	}
	serv = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (serv < 0) {
		LOG_ERR("udp socket create error");
		return;
	}

	bind_addr.sin_family = AF_INET;
	bind_addr.sin_addr.s_addr = INADDR_ANY;
	bind_addr.sin_port = htons(MULTICAST_PORT);
	if (bind(serv, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
		LOG_ERR("error: bind: %d", errno);
		return;
	}

	while (1) {
		client_addr_len = sizeof(client_addr);
		buf_len = recvfrom(serv, udp_buffer, sizeof(udp_buffer), 0,
				   (struct sockaddr *)&client_addr, &client_addr_len);
		if (buf_len > 0) {
			udp_buffer[buf_len] = '\0';
			LOG_DBG("udp data: %s", udp_buffer);
		} else {
			LOG_ERR("udp error!");
			continue;
		}
		buf_len = parse_udp_msg(udp_buffer, sizeof(udp_buffer));
		net_addr_pton(AF_INET, "224.0.0.1", &client_addr.sin_addr);
		if (sendto(serv, udp_buffer, buf_len, 0, (struct sockaddr *)&client_addr,
			   sizeof(struct sockaddr)) == -1) {
			LOG_ERR("udp send message error: %d!", errno);
		}

		k_msleep(10);
	}
}

K_THREAD_DEFINE(udp_bcast, CONFIG_UDP_STACK_SIZE, udp_poll, NULL, NULL, NULL, CONFIG_UDP_PRIORITY,
		0, 0);
