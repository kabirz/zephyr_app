#include <zephyr/kernel.h>
#include <zephyr/posix/sys/select.h>
#include <unistd.h>
#include "ftp.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ftp, LOG_LEVEL_INF);

#define WELCOME_MSG     "220 Light FTP Server\r\n"
#define CONNECT_ERR_MSG "Only 3 connections is allowed, please close other one and retry\r\n"

#define MAX_CLIENTS 4
static uint8_t data_buf[256];
static struct ftp_session ftp_sessions[MAX_CLIENTS];

static void ftp_session_reset(struct ftp_session *sess)
{
	memset(sess, 0, sizeof(*sess));
	sess->port_pasv_fd = -1;
	sess->pasvs_fd = -1;
};

static void ftp_session_release(struct ftp_session *sess)
{
	close(sess->fd);
	if (sess->port_pasv_fd != -1) {
		close(sess->port_pasv_fd);
	}
	if (sess->pasvs_fd != -1) {
		close(sess->pasvs_fd);
	}
	ftp_session_reset(sess);
}

static void ftp_poll(void)
{
	int serv, max_fd;
	struct sockaddr_in bind_addr;
	struct timeval timeout;
	static int counter;
	fd_set readfds;

	serv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (serv < 0) {
		LOG_ERR("error: socket: %d", errno);
		return;
	}

	bind_addr.sin_family = AF_INET;
	bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	bind_addr.sin_port = htons(FTP_PORT);

	if (bind(serv, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
		LOG_ERR("error: bind: %d", errno);
		return;
	}

	if (listen(serv, 5) < 0) {
		LOG_ERR("error: listen: %d", errno);
		return;
	}

	LOG_INF("Started ftp server on port %d", FTP_PORT);

	while (1) {
		struct sockaddr_in client_addr;
		socklen_t client_addr_len = sizeof(client_addr);
		char addr_str[INET_ADDRSTRLEN];
		uint32_t connect_num = 0;
		int rc;

		FD_ZERO(&readfds);
		FD_SET(serv, &readfds);
		max_fd = serv;
		for (int i = 0; i < MAX_CLIENTS; i++) {
			if (ftp_sessions[i].fd > 0) {
				FD_SET(ftp_sessions[i].fd, &readfds);
				connect_num++;
			}
			if (ftp_sessions[i].fd > max_fd) {
				max_fd = ftp_sessions[i].fd;
			}
		}

		timeout.tv_sec = 1;
		timeout.tv_usec = 0;

		rc = select(max_fd + 1, &readfds, NULL, NULL, &timeout);
		if ((rc < 0) && (errno != EINTR)) {
			LOG_ERR("out of poll max fd");
			k_msleep(100);
			for (int i = 0; i < MAX_CLIENTS; i++) {
				if (ftp_sessions[i].time &&
				    (ftp_sessions[i].time + FTP_SESSION_TIMEOUT) < k_uptime_get()) {
					ftp_session_release(&ftp_sessions[i]);
					connect_num--;
				}
			}

			continue;
		} else if (rc == 0) {
			continue;
		}

		if (FD_ISSET(serv, &readfds)) {
			int client =
				accept(serv, (struct sockaddr *)&client_addr, &client_addr_len);

			if (client < 0) {
				LOG_ERR("error: accept: %d", errno);
				continue;
			}

			if (connect_num > 2) {
				LOG_WRN("Allow 2 connections at same time, wait...");
				if (send(client, CONNECT_ERR_MSG, strlen(CONNECT_ERR_MSG), 0) < 0) {
					LOG_ERR("send error");
				}
				close(client);
				continue;
			}

			for (int i = 0; i < MAX_CLIENTS; i++) {
				if (ftp_sessions[i].fd == 0) {
					ftp_sessions[i].fd = client;
					ftp_sessions[i].port_pasv_fd = -1;
					ftp_sessions[i].pasvs_fd = -1;
					ftp_sessions[i].time = k_uptime_get();
					getpeername(ftp_sessions[i].fd,
						    (struct sockaddr *)&client_addr,
						    (socklen_t *)&client_addr_len);
					inet_ntop(client_addr.sin_family, &client_addr.sin_addr,
						  addr_str, sizeof(addr_str));
					LOG_INF("Host(%s:%d) connected, counts: %d", addr_str,
						ntohs(client_addr.sin_port), ++counter);
					LOG_INF("Adding to list of sockets as %d", i);

					if (send(client, WELCOME_MSG, strlen(WELCOME_MSG), 0) < 0) {
						LOG_ERR("send error");
					}

					connect_num++;
					break;
				} else if (i == MAX_CLIENTS - 1) {
					close(client);
				}
			}
		} else {
			for (int i = 0; i < MAX_CLIENTS; i++) {
				if (FD_ISSET(ftp_sessions[i].fd, &readfds)) {
					if ((rc = recv(ftp_sessions[i].fd, data_buf,
						       sizeof(data_buf), 0)) == 0) {
						getpeername(ftp_sessions[i].fd,
							    (struct sockaddr *)&client_addr,
							    (socklen_t *)&client_addr_len);
						LOG_INF("Host(%s:%d) close connection",
							inet_ntoa(client_addr.sin_addr),
							ntohs(client_addr.sin_port));
						ftp_session_release(&ftp_sessions[i]);
						connect_num--;
					} else {
						ftp_sessions[i].time = k_uptime_get();
						data_buf[rc - 2] = '\0';
						if (ftp_proccess(&ftp_sessions[i], data_buf) < 0) {
							LOG_ERR("send error");
						}
					}
				}
			}
		}
	}
}

K_THREAD_DEFINE(ftp, CONFIG_FTP_STACK_SIZE, ftp_poll, NULL, NULL, NULL, CONFIG_FTP_PRIORITY, 0, 0);
