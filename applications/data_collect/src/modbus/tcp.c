/*
 * Copyright (c) 2024 kabirz
 * SPDX-License-Identifier: Apache-2.0
 */

#include "init.h"
#include <zephyr/modbus/modbus.h>
#include <zephyr/posix/sys/select.h>
#include <zephyr/net/socket.h>

#define MAX_CLIENTS 3
#define MODBUS_TCP_PORT 502
static uint8_t data_buf[256];
static struct modbus_client {
    int fd;
    int64_t time;
} mb_clients[MAX_CLIENTS];

static struct modbus_adu tmp_adu;
K_SEM_DEFINE(received, 0, 1);
static int server_iface;

static int server_raw_cb(const int iface, const struct modbus_adu *adu,
			 void *user_data)
{
    LOG_DBG("Server raw callback from interface %d", iface);

    tmp_adu.trans_id = adu->trans_id;
    tmp_adu.proto_id = adu->proto_id;
    tmp_adu.length = adu->length;
    tmp_adu.unit_id = adu->unit_id;
    tmp_adu.fc = adu->fc;
    memcpy(tmp_adu.data, adu->data,
	   MIN(adu->length, CONFIG_MODBUS_BUFFER_SIZE));

    LOG_HEXDUMP_DBG(tmp_adu.data, tmp_adu.length, "resp");
    k_sem_give(&received);

    return 0;
}

static struct modbus_iface_param server_param = {
    .mode = MODBUS_MODE_RAW,
    .server = {
	.user_cb = &mbs_cbs,
	.unit_id = 1,
    },
    .rawcb.raw_tx_cb = server_raw_cb,
    .rawcb.user_data = NULL
};

static int init_modbus_server(void)
{
    char iface_name[] = "RAW_0";
    int err;

    server_iface = modbus_iface_get_by_name(iface_name);

    if (server_iface < 0) {
	LOG_ERR("Failed to get iface index for %s",
	 iface_name);
	return -ENODEV;
    }
    server_param.server.unit_id = get_holding_reg(HOLDING_SLAVE_ID_IDX);
    err = modbus_init_server(server_iface, server_param);

    if (err < 0) {
	return err;
    }
    return err;
}

void tcp_poll(void)
{
    int serv, max_fd;
    struct sockaddr_in bind_addr;
    struct timeval timeout;
    static int counter;
    fd_set readfds;

    if (init_modbus_server()) {
        LOG_ERR("Modbus TCP server initialization failed");
        return;
    }

    serv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serv < 0) {
        LOG_ERR("error: socket: %d", errno);
        return;
    }

    bind_addr.sin_family = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port = htons(MODBUS_TCP_PORT);

    if (bind(serv, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
        LOG_ERR("error: bind: %d", errno);
        return;
    }

    if (listen(serv, 5) < 0) {
        LOG_ERR("error: listen: %d", errno);
        return;
    }

    LOG_INF("Started MODBUS TCP server on port %d", MODBUS_TCP_PORT);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        char addr_str[INET_ADDRSTRLEN];
        bool had_connect = false;
        int rc;

        FD_ZERO(&readfds);
        FD_SET(serv, &readfds);
        max_fd = serv;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (mb_clients[i].fd > 0) {
                FD_SET(mb_clients[i].fd, &readfds);
                had_connect = true;
            }
            if (mb_clients[i].fd > max_fd)
                max_fd = mb_clients[i].fd;
        }

        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        rc = select(max_fd + 1, &readfds, NULL, NULL, &timeout);
        if ((rc < 0) && (errno != EINTR)) {
            LOG_ERR("out of poll max fd");
            k_msleep(100);
            continue;
        } else if (rc == 0) {
            continue;
        }

        if (FD_ISSET(serv, &readfds)) {
            int client = accept(serv, (struct sockaddr *)&client_addr, &client_addr_len);

            if (client < 0) {
                LOG_ERR("error: accept: %d", errno);
                continue;
            }

            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (mb_clients[i].time && (mb_clients[i].time + 30000) < k_uptime_get()) {
                    getpeername(mb_clients[i].fd, (struct sockaddr *)&client_addr, (socklen_t *)&client_addr_len);
                    LOG_INF("Host(%s:%d) connection is terminated due to timeout by others", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
                    close(mb_clients[i].fd);
                    had_connect = false;
                    mb_clients[i].fd = 0;
                    mb_clients[i].time = 0;
                }
            }

            if (had_connect) {
                LOG_WRN("Only allow one connection at same time, wait...");
                close(client);
                continue;
            }

            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (mb_clients[i].fd == 0) {
                    mb_clients[i].fd = client;
                    mb_clients[i].time = k_uptime_get();
                    getpeername(mb_clients[i].fd, (struct sockaddr *)&client_addr, (socklen_t *)&client_addr_len);
                    inet_ntop(client_addr.sin_family, &client_addr.sin_addr, addr_str, sizeof(addr_str));
                    LOG_INF("Host(%s:%d) connected, counts: %d", addr_str, ntohs(client_addr.sin_port), ++counter);
                    LOG_INF("Adding to list of sockets as %d", i);
                    break;
                } else if (i == MAX_CLIENTS - 1)
                    close(client);
            }
        } else {
            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (FD_ISSET(mb_clients[i].fd, &readfds)) {
                    if ((rc = recv(mb_clients[i].fd, data_buf, MODBUS_MBAP_AND_FC_LENGTH, MSG_WAITALL)) == 0) {
                        getpeername(mb_clients[i].fd, (struct sockaddr *)&client_addr, (socklen_t *)&client_addr_len);
                        LOG_INF("Host(%s:%d) close connection", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
                        close(mb_clients[i].fd);
                        mb_clients[i].fd = 0;
                        mb_clients[i].time = 0;
                    } else {
                        int data_len;

                        mb_clients[i].time = k_uptime_get();
                        LOG_HEXDUMP_DBG(data_buf, MODBUS_MBAP_AND_FC_LENGTH, "h:>");
                        modbus_raw_get_header(&tmp_adu, data_buf);
                        data_len = tmp_adu.length;
                        if ((rc = recv(mb_clients[i].fd, tmp_adu.data, tmp_adu.length, MSG_WAITALL) < 0)) {
                            LOG_ERR("receive data error");
                            break;
                        }

                        LOG_HEXDUMP_DBG(tmp_adu.data, tmp_adu.length, "d:>");
                        if (modbus_raw_submit_rx(server_iface, &tmp_adu)) {
                            LOG_ERR("Failed to submit raw ADU");
                            continue;
                        }

                        if (k_sem_take(&received, K_MSEC(1000)) != 0) {
                            LOG_ERR("MODBUS RAW wait time expired");
                            modbus_raw_set_server_failure(&tmp_adu);
                        }
                        modbus_raw_put_header(&tmp_adu, data_buf);
                        memcpy(data_buf + MODBUS_MBAP_AND_FC_LENGTH, tmp_adu.data, tmp_adu.length);

                        if (send(mb_clients[i].fd, data_buf, MODBUS_MBAP_AND_FC_LENGTH + tmp_adu.length, 0) < 0) {
                            LOG_ERR("send error");
                        }
                    }
                }
            }
        }
    }
}

K_THREAD_DEFINE(mb_tcp, CONFIG_MODBUS_TCP_STACK_SIZE, tcp_poll, NULL, NULL, NULL, CONFIG_MODBUS_TCP_PRIORITY, 0, 0);
