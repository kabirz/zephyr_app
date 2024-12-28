#include <zephyr/posix/sys/select.h>
#include <zephyr/posix/dirent.h>
#include <zephyr/posix/fcntl.h>
#include <zephyr/posix/unistd.h>
#include <zephyr/posix/sys/time.h>
#include <zephyr/net/net_l2.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/net_if.h>
#include "ftp.h"

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(ftp, LOG_LEVEL_INF);

static int ftp_create_dir(const char *path)
{
	int result = 0;

	DIR *dir = opendir(path);
	if (dir == NULL) {
		if (mkdir(path, 0x777) != 0) {
			result = -1;
		}
	} else {
		closedir(dir);
	}

	return result;
}

static char *ftp_normalize_path(char *fullpath)
{
	char *dst0, *dst, *src;

	src = fullpath;
	dst = fullpath;

	dst0 = dst;
	while (1) {
		char c = *src;

		if (c == '.') {
			if (!src[1]) {
				src++; /* '.' and ends */
			} else if (src[1] == '/') {
				/* './' case */
				src += 2;

				while ((*src == '/') && (*src != '\0')) {src++;}
				continue;
			} else if (src[1] == '.') {
				if (!src[2]) {
					/* '..' and ends case */
					src += 2;
					goto up_one;
				} else if (src[2] == '/') {
					/* '../' case */
					src += 3;

					while ((*src == '/') && (*src != '\0')) {src++;}
					goto up_one;
				}
			}
		}

		/* copy up the next '/' and erase all '/' */
		while ((c = *src++) != '\0' && c != '/') {*dst++ = c;}

		if (c == '/') {
			*dst++ = '/';
			while (c == '/') {c = *src++;}

			src--;
		} else if (!c) {
			break;
		}

		continue;

up_one:
		dst--;
		if (dst < dst0) {
			return NULL;
		}
		while (dst0 < dst && dst[-1] != '/') {
			dst--;
		}
	}

	*dst = '\0';

	/* remove '/' in the end of path if exist */
	dst--;
	if ((dst != fullpath) && (*dst == '/')) {
		*dst = '\0';
	}

	return fullpath;
}

static int port_cmd_fn(struct ftp_session *session, char *cmd, char *cmd_param)
{
	if (session->port_pasv_fd >= 0) {
		close(session->port_pasv_fd);
		session->port_pasv_fd = -1;
		if (session->pasvs_fd > 0) {
			close(session->pasvs_fd);
			session->pasvs_fd = -1;
		}
	}

	char *reply = NULL;
	int portcom[6];
	char iptmp[100];
	int index = 0;
	int port = 0;
	char *ptr = cmd_param;
	if (strcmp(cmd, "PORT") == 0) {
		/* format ip1,ip2,ip3,ip4,porth,portl */
		while (ptr != NULL) {
			if (*ptr == ',') {
				ptr++;
			}
			portcom[index] = atoi(ptr);
			if ((portcom[index] < 0) || (portcom[index] > 255)) {
				break;
			}
			index++;
			if (index == 6) {
				break;
			}
			ptr = strchr(ptr, ',');
		}
		if (index < 6) {
			reply = "504 invalid parameter.\r\n";
			send(session->fd, reply, strlen(reply), 0);
			return 0;
		}

		snprintf(iptmp, sizeof(iptmp), "%d.%d.%d.%d", portcom[0], portcom[1], portcom[2],
			 portcom[3]);
		port = portcom[4] << 8 | portcom[5];
	} else if (strcmp(cmd, "EPRT") == 0) {
		/* format |<type>|<ip>|<port>| */
		char *split;
		ptr++;
		if (*ptr != '1') {
			reply = "425 Only support IPv4.\r\n";
			send(session->fd, reply, strlen(reply), 0);
			return 0;
		}
		ptr += 2;
		split = strchr(ptr, '|');
		*split = '\0';
		snprintf(iptmp, sizeof(iptmp), "%s", ptr);
		ptr = split + 1;
		split = strchr(ptr, '|');
		*split = '\0';
		port = atoi(ptr);
	}

	int rc = -1;
	do {
		session->port_pasv_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (session->port_pasv_fd < 0) {
			LOG_ERR("socket create error");
			break;
		}
		struct timeval tv;
		tv.tv_sec = 20;
		tv.tv_usec = 0;
		if (setsockopt(session->port_pasv_fd, SOL_SOCKET, SO_SNDTIMEO, (const void *)&tv,
			       sizeof(struct timeval)) < 0) {
			LOG_ERR("don't support SO_SNDTIMEO");
			break;
		}
		struct sockaddr_in addr;
		memset(&addr, 0, sizeof(addr));
		addr.sin_family = AF_INET;
		addr.sin_port = htons(port);
		addr.sin_addr.s_addr = inet_addr(iptmp);
		if (connect(session->port_pasv_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
			LOG_ERR("connect error");
			break;
		}

		rc = 0;
	} while (0);

	if (rc != 0) {
		reply = "425 Can't open data connection.\r\n";
		send(session->fd, reply, strlen(reply), 0);
		if (session->port_pasv_fd >= 0) {
			close(session->port_pasv_fd);
			session->port_pasv_fd = -1;
			if (session->pasvs_fd > 0) {
				close(session->pasvs_fd);
				session->pasvs_fd = -1;
			}
		}
		return 0;
	}

	reply = "200 Port Command Successful.\r\n";
	send(session->fd, reply, strlen(reply), 0);
	return 0;
}

static int pasv_cmd_fn(struct ftp_session *session, char *cmd, char *cmd_param)
{
	struct sockaddr_in bind_addr;
	socklen_t addr_len = sizeof(bind_addr);
	char *reply = malloc(1024);

	session->pasvs_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (session->pasvs_fd < 0) {
		snprintf(reply, 1024, "504 socket create error.\r\n");
		goto passv_end;
	}

	bind_addr.sin_family = AF_INET;
	bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
	bind_addr.sin_port = 0;

	if (bind(session->pasvs_fd, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
		snprintf(reply, 1024, "504 socket bind error.\r\n");
		goto passv_end;
	}
	if (getsockname(session->pasvs_fd, (struct sockaddr *)&bind_addr, &addr_len) != 0) {
		LOG_ERR("Failed to get socket name");
	}

	char addr_str[INET_ADDRSTRLEN];
	inet_ntop(bind_addr.sin_family, &bind_addr.sin_addr, addr_str, sizeof(addr_str));
	if (listen(session->pasvs_fd, 5) < 0) {
		snprintf(reply, 1024, "504 socket listen error.\r\n");
		goto passv_end;
	}

	if (strcmp(cmd, "PASV") == 0) {
		struct net_if *iface = net_if_get_first_by_type(&NET_L2_GET_NAME(ETHERNET));
		if (iface) {
			uint8_t *addr = iface->config.ip.ipv4->unicast->ipv4.address.in_addr.s4_addr;
			snprintf(reply, 1024, "227 Entering Passive Mode (%d,%d,%d,%d,%d,%d).\r\n",
				addr[0], addr[1], addr[2], addr[3],
				bind_addr.sin_port & 0xff,
				bind_addr.sin_port >> 8);
		}
	} else if (strcmp(cmd, "EPSV") == 0) {
		snprintf(reply, 1024, "229 Entering Extended Passive Mode (|||%d|).\r\n",
			 ntohs(bind_addr.sin_port));
	}
passv_end:
	send(session->fd, reply, strlen(reply), 0);
	free(reply);

	return 0;
}

static int pwd_cmd_fn(struct ftp_session *session, char *cmd, char *cmd_param)
{
	char *reply = malloc(1024);
	if (reply == NULL) {
		return -1;
	}

	snprintf(reply, 1024, "257 \"%s\" is current directory.\r\n", session->currentdir);
	send(session->fd, reply, strlen(reply), 0);
	free(reply);
	return 0;
}

static int type_cmd_fn(struct ftp_session *session, char *cmd, char *cmd_param)
{
	// Ignore it
	char *reply = NULL;
	if (strcmp(cmd_param, "I") == 0) {
		reply = "200 Type set to binary.\r\n";
	} else {
		reply = "200 Type set to ascii.\r\n";
	}

	send(session->fd, reply, strlen(reply), 0);
	return 0;
}

static int syst_cmd_fn(struct ftp_session *session, char *cmd, char *cmd_param)
{
	char *reply = "215 RT-Thread RTOS\r\n";
	send(session->fd, reply, strlen(reply), 0);
	return 0;
}

static int quit_cmd_fn(struct ftp_session *session, char *cmd, char *cmd_param)
{
	char *reply = "221 Bye!\r\n";
	send(session->fd, reply, strlen(reply), 0);

	return -1;
}

static int list_statbuf_get(struct dirent *dirent, struct stat *s, char *buf, int bufsz)
{
	int ret = 0;
	struct tm ftm;
	struct tm ntm;
	time_t now_time;

	// file type
	memset(buf, '-', 10);

	if (S_ISDIR(s->st_mode)) {
		buf[0] = 'd';
	}

	if (s->st_mode & S_IRUSR) {
		buf[1] = 'r';
	}
	if (s->st_mode & S_IWUSR) {
		buf[2] = 'w';
	}
	if (s->st_mode & S_IXUSR) {
		buf[3] = 'x';
	}

	if (s->st_mode & S_IRGRP) {
		buf[4] = 'r';
	}
	if (s->st_mode & S_IWGRP) {
		buf[5] = 'w';
	}
	if (s->st_mode & S_IXGRP) {
		buf[6] = 'x';
	}

	if (s->st_mode & S_IROTH) {
		buf[7] = 'r';
	}
	if (s->st_mode & S_IWOTH) {
		buf[8] = 'w';
	}
	if (s->st_mode & S_IXOTH) {
		buf[9] = 'x';
	}

	ret += 10;
	buf[ret++] = ' ';

	// user info
	ret += snprintf(buf + ret, bufsz - ret, "%d %s %s", s->st_nlink, "admin", "admin");
	buf[ret++] = ' ';

	// file size
	ret += snprintf(buf + ret, bufsz - ret, "%ld", s->st_size);
	buf[ret++] = ' ';

	// file date
	gmtime_r(&s->st_mtime, &ftm);
	now_time = time(NULL);
	gmtime_r(&now_time, &ntm);

	if (ftm.tm_year == ntm.tm_year) {
		ret += strftime(buf + ret, bufsz - ret, "%b %d %H:%M", &ftm);
	} else {
		ret += strftime(buf + ret, bufsz - ret, "%b %d %Y", &ftm);
	}

	buf[ret++] = ' ';

	// file name
	ret += snprintf(buf + ret, bufsz - ret, "%s\r\n", dirent->d_name);

	return ret;
}

void pasv_accept(struct ftp_session *session)
{
	struct sockaddr_in client_addr;
	socklen_t client_addr_len = sizeof(client_addr);

	session->port_pasv_fd = accept(session->pasvs_fd, (struct sockaddr *)&client_addr, &client_addr_len);
}

static int list_cmd_fn(struct ftp_session *session, char *cmd, char *cmd_param)
{
	char *reply = NULL;

	if (session->pasvs_fd > 0) {
		pasv_accept(session);
	}

	if (session->port_pasv_fd < 0) {
		reply = "502 Not Implemented.\r\n";
		send(session->fd, reply, strlen(reply), 0);
		return 0;
	}

	DIR *dir = opendir(session->currentdir);
	if (dir == NULL) {
		reply = malloc(1024);
		if (reply == NULL) {
			return -1;
		}

		snprintf(reply, 1024, "550 directory \"%s\" can't open.\r\n", session->currentdir);
		send(session->fd, reply, strlen(reply), 0);
		free(reply);
		return 0;
	}

	reply = "150 Opening Binary mode connection for file list.\r\n";
	send(session->fd, reply, strlen(reply), 0);

	struct dirent *dirent = NULL;
	char tmp[256];
	struct stat s;
	do {
		dirent = readdir(dir);
		if (dirent == NULL) {
			break;
		}
		snprintf(tmp, sizeof(tmp), "%s/%s", session->currentdir, dirent->d_name);
		memset(&s, 0, sizeof(struct stat));
		if (stat(tmp, &s) != 0) {
			continue;
		}

		int stat_len = list_statbuf_get(dirent, &s, tmp, sizeof(tmp));
		if (stat_len <= 0) {
			continue;
		}

		send(session->port_pasv_fd, tmp, stat_len, 0);
	} while (dirent != NULL);

	closedir(dir);

	close(session->port_pasv_fd);
	session->port_pasv_fd = -1;
	if (session->pasvs_fd > 0) {
		close(session->pasvs_fd);
		session->pasvs_fd = -1;
	}

	reply = "226 Transfert Complete.\r\n";
	send(session->fd, reply, strlen(reply), 0);
	return 0;
}

static int nlist_cmd_fn(struct ftp_session *session, char *cmd, char *cmd_param)
{
	char *reply = NULL;

	if (session->pasvs_fd > 0) {
		pasv_accept(session);
	}

	if (session->port_pasv_fd < 0) {
		reply = "502 Not Implemented.\r\n";
		send(session->fd, reply, strlen(reply), 0);
		return 0;
	}

	DIR *dir = opendir(session->currentdir);
	if (dir == NULL) {
		reply = malloc(1024);
		if (reply == NULL) {
			return -1;
		}

		snprintf(reply, 1024, "550 directory \"%s\" can't open.\r\n", session->currentdir);
		send(session->fd, reply, strlen(reply), 0);
		free(reply);
		return 0;
	}

	reply = "150 Opening Binary mode connection for file list.\r\n";
	send(session->fd, reply, strlen(reply), 0);

	struct dirent *dirent = NULL;
	char tmp[256];
	do {
		dirent = readdir(dir);
		if (dirent == NULL) {
			break;
		}
		snprintf(tmp, sizeof(tmp), "%s\r\n", dirent->d_name);
		send(session->port_pasv_fd, tmp, strlen(tmp), 0);
	} while (dirent != NULL);

	closedir(dir);

	close(session->port_pasv_fd);
	session->port_pasv_fd = -1;
	if (session->pasvs_fd > 0) {
		close(session->pasvs_fd);
		session->pasvs_fd = -1;
	}

	reply = "226 Transfert Complete.\r\n";
	send(session->fd, reply, strlen(reply), 0);
	return 0;
}

static int build_full_path(char *buf, int bufsz, const char *path)
{
	if (path[0] == '/') {
		snprintf(buf, bufsz, "%s", path);
	} else {
		strcat(buf, "/");
		int remain_len = bufsz - strlen(buf) - 1;
		strncat(buf, path, remain_len);
	}

	if (ftp_normalize_path(buf) == NULL) {
		return -1;
	}

	return 0;
}

static int cwd_cmd_fn(struct ftp_session *session, char *cmd, char *cmd_param)
{
	if (build_full_path(session->currentdir, sizeof(session->currentdir), cmd_param) != 0) {
		return -1;
	}

	char *reply = NULL;
	DIR *dir = opendir(session->currentdir);
	if (dir == NULL) {
		reply = malloc(1024);
		if (reply == NULL) {
			return -1;
		}

		snprintf(reply, 1024, "550 directory \"%s\" can't open.\r\n", session->currentdir);
		send(session->fd, reply, strlen(reply), 0);
		free(reply);
		return 0;
	}

	closedir(dir);

	reply = malloc(1024);
	if (reply == NULL) {
		return -1;
	}

	snprintf(reply, 1024, "250 Changed to directory \"%s\"\r\n", session->currentdir);
	send(session->fd, reply, strlen(reply), 0);
	free(reply);
	return 0;
}

static int cdup_cmd_fn(struct ftp_session *session, char *cmd, char *cmd_param)
{
	if (build_full_path(session->currentdir, sizeof(session->currentdir), "..") != 0) {
		return -1;
	}

	char *reply = NULL;
	DIR *dir = opendir(session->currentdir);
	if (dir == NULL) {
		reply = malloc(1024);
		if (reply == NULL) {
			return -1;
		}

		snprintf(reply, 1024, "550 directory \"%s\" can't open.\r\n", session->currentdir);
		send(session->fd, reply, strlen(reply), 0);
		free(reply);
		return 0;
	}

	closedir(dir);

	reply = malloc(1024);
	if (reply == NULL) {
		return -1;
	}

	snprintf(reply, 1024, "250 Changed to directory \"%s\"\r\n", session->currentdir);
	send(session->fd, reply, strlen(reply), 0);
	free(reply);
	return 0;
}

static int mkd_cmd_fn(struct ftp_session *session, char *cmd, char *cmd_param)
{
	char *reply = NULL;
	if (session->is_anonymous) {
		reply = "550 Permission denied.\r\n";
		send(session->fd, reply, strlen(reply), 0);
		return 0;
	}

	char path[256];
	snprintf(path, sizeof(path), "%s", session->currentdir);
	if (build_full_path(path, sizeof(path), cmd_param) != 0) {
		return -1;
	}

	reply = malloc(1024);
	if (reply == NULL) {
		return -1;
	}

	if (ftp_create_dir(path) != 0) {
		snprintf(reply, 1024, "550 directory \"%s\" create error.\r\n", path);
	} else {
		snprintf(reply, 1024, "257 directory \"%s\" successfully created.\r\n", path);
	}

	send(session->fd, reply, strlen(reply), 0);
	free(reply);
	return 0;
}

static int rmd_cmd_fn(struct ftp_session *session, char *cmd, char *cmd_param)
{
	char *reply = NULL;
	if (session->is_anonymous) {
		reply = "550 Permission denied.\r\n";
		send(session->fd, reply, strlen(reply), 0);
		return 0;
	}

	char path[256];
	snprintf(path, sizeof(path), "%s", session->currentdir);
	if (build_full_path(path, sizeof(path), cmd_param) != 0) {
		return -1;
	}

	reply = malloc(1024);
	if (reply == NULL) {
		return -1;
	}

	if (unlink(path) != 0) {
		snprintf(reply, 1024, "550 directory \"%s\" delete error.\r\n", path);
	} else {
		snprintf(reply, 1024, "257 directory \"%s\" successfully deleted.\r\n", path);
	}

	send(session->fd, reply, strlen(reply), 0);
	free(reply);
	return 0;
}

static int dele_cmd_fn(struct ftp_session *session, char *cmd, char *cmd_param)
{
	char *reply = NULL;
	if (session->is_anonymous) {
		reply = "550 Permission denied.\r\n";
		send(session->fd, reply, strlen(reply), 0);
		return 0;
	}

	char path[256];
	snprintf(path, sizeof(path), "%s", session->currentdir);
	if (build_full_path(path, sizeof(path), cmd_param) != 0) {
		return -1;
	}

	reply = malloc(1024);
	if (reply == NULL) {
		return -1;
	}

	if (unlink(path) != 0) {
		snprintf(reply, 1024, "550 file \"%s\" delete error.\r\n", path);
	} else {
		snprintf(reply, 1024, "250 file \"%s\" successfully deleted.\r\n", path);
	}

	send(session->fd, reply, strlen(reply), 0);
	free(reply);
	return 0;
}

static int size_cmd_fn(struct ftp_session *session, char *cmd, char *cmd_param)
{
	char *reply = NULL;
	char path[256];
	snprintf(path, sizeof(path), "%s", session->currentdir);
	if (build_full_path(path, sizeof(path), cmd_param) != 0) {
		return -1;
	}

	struct stat s;
	memset(&s, 0, sizeof(struct stat));
	if (stat(path, &s) != 0) {
		reply = malloc(1024);
		if (reply == NULL) {
			return -1;
		}

		snprintf(reply, 1024, "550 \"%s\" : not a regular file\r\n", path);
		send(session->fd, reply, strlen(reply), 0);
		free(reply);
		return 0;
	}

	if (!S_ISREG(s.st_mode)) {
		reply = malloc(1024);
		if (reply == NULL) {
			return -1;
		}

		snprintf(reply, 1024, "550 \"%s\" : not a regular file\r\n", path);
		send(session->fd, reply, strlen(reply), 0);
		free(reply);
		return 0;
	}

	reply = malloc(1024);
	if (reply == NULL) {
		return -1;
	}

	snprintf(reply, 1024, "213 %ld\r\n", s.st_size);
	send(session->fd, reply, strlen(reply), 0);
	free(reply);
	return 0;
}

static int rest_cmd_fn(struct ftp_session *session, char *cmd, char *cmd_param)
{
	char *reply = NULL;

	int offset = atoi(cmd_param);
	if (offset < 0) {
		reply = "504 invalid parameter.\r\n";
		send(session->fd, reply, strlen(reply), 0);
		session->offset = 0;
		return 0;
	}

	reply = "350 Send RETR or STOR to start transfert.\r\n";
	send(session->fd, reply, strlen(reply), 0);
	session->offset = offset;
	return 0;
}

static int retr_cmd_fn(struct ftp_session *session, char *cmd, char *cmd_param)
{
	char *reply = NULL;

	if (session->pasvs_fd > 0) {
		pasv_accept(session);
	}

	if (session->port_pasv_fd < 0) {
		reply = "502 Not Implemented.\r\n";
		send(session->fd, reply, strlen(reply), 0);
		session->offset = 0;
		return 0;
	}

	char path[256];
	snprintf(path, sizeof(path), "%s", session->currentdir);
	if (build_full_path(path, sizeof(path), cmd_param) != 0) {
		return -1;
	}

	int fd = open(path, O_RDONLY);
	if (fd < 0) {
		reply = malloc(1024);
		if (reply == NULL) {
			return -1;
		}

		snprintf(reply, 1024, "550 \"%s\" : not a regular file\r\n", path);
		send(session->fd, reply, strlen(reply), 0);
		free(reply);
		session->offset = 0;
		return 0;
	}

	int rc = -1;
	int file_size = 0;
	do {
		file_size = lseek(fd, 0, SEEK_END);
		lseek(fd, 0, SEEK_SET);
		if (file_size <= 0) {
			break;
		}

		rc = 0;
	} while (0);

	if (rc != 0) {
		close(fd);

		reply = malloc(1024);
		if (reply == NULL) {
			return -1;
		}

		snprintf(reply, 1024, "550 \"%s\" : not a regular file\r\n", path);
		send(session->fd, reply, strlen(reply), 0);
		free(reply);
		session->offset = 0;
		return 0;
	}

	reply = malloc(4096);
	if (reply == NULL) {
		close(fd);
		return -1;
	}

	if ((session->offset > 0) && (session->offset < file_size)) {
		lseek(fd, session->offset, SEEK_SET);
		snprintf(reply, 4096,
			 "150 Opening binary mode data connection for \"%s\" (%d/%d bytes).\r\n",
			 path, file_size - session->offset, file_size);
	} else {
		snprintf(reply, 4096,
			 "150 Opening binary mode data connection for \"%s\" (%d bytes).\r\n", path,
			 file_size);
	}
	send(session->fd, reply, strlen(reply), 0);

	int recv_bytes = 0;
	int result = 0;

	while ((recv_bytes = read(fd, reply, 4096)) > 0) {
		int send_bytes = 0;
		while (send_bytes < recv_bytes) {
			result = send(session->port_pasv_fd, reply + send_bytes,
				      recv_bytes - send_bytes, 0);
			if (result <= 0) {
				goto out;
			}
			send_bytes += result;
		}
	}
out:
	free(reply);
	close(fd);
	close(session->port_pasv_fd);
	session->port_pasv_fd = -1;
	if (session->pasvs_fd > 0) {
		close(session->pasvs_fd);
		session->pasvs_fd = -1;
	}

	if (result != 0) {
		return -1;
	}

	reply = "226 Finished.\r\n";
	send(session->fd, reply, strlen(reply), 0);
	session->offset = 0;
	return 0;
}

static int stor_cmd_receive(int socket, uint8_t *buf, int bufsz, int timeout)
{
	if ((socket < 0) || (buf == NULL) || (bufsz <= 0) || (timeout <= 0)) {
		return -1;
	}

	int len = 0;
	int rc = 0;
	fd_set rset;
	struct timeval tv;

	FD_ZERO(&rset);
	FD_SET(socket, &rset);
	tv.tv_sec = timeout / 1000;
	tv.tv_usec = (timeout % 1000) * 1000;

	while (bufsz > 0) {
		rc = select(socket + 1, &rset, NULL, NULL, &tv);
		if (rc <= 0) {
			break;
		}

		rc = recv(socket, buf + len, bufsz, MSG_DONTWAIT);
		if (rc <= 0) {
			break;
		}

		len += rc;
		bufsz -= rc;

		tv.tv_sec = 3;
		tv.tv_usec = 0;
		FD_ZERO(&rset);
		FD_SET(socket, &rset);
	}

	if (rc >= 0) {
		rc = len;
	}

	return rc;
}

static int stor_cmd_fn(struct ftp_session *session, char *cmd, char *cmd_param)
{
	session->offset = 0;

	char *reply = NULL;
	if (session->is_anonymous) {
		reply = "550 Permission denied.\r\n";
		send(session->fd, reply, strlen(reply), 0);
		return 0;
	}

	if (session->pasvs_fd > 0) {
		pasv_accept(session);
	}

	if (session->port_pasv_fd < 0) {
		reply = "502 Not Implemented.\r\n";
		send(session->fd, reply, strlen(reply), 0);
		return 0;
	}

	char path[256];
	snprintf(path, sizeof(path), "%s", session->currentdir);
	if (build_full_path(path, sizeof(path), cmd_param) != 0) {
		return -1;
	}

	int fd = open(path, O_CREAT | O_RDWR | O_TRUNC);
	if (fd < 0) {
		reply = malloc(1024);
		if (reply == NULL) {
			return -1;
		}

		snprintf(reply, 1024, "550 Cannot open \"%s\" for writing.\r\n", path);
		send(session->fd, reply, strlen(reply), 0);
		free(reply);
		return 0;
	}

	reply = malloc(4096);
	if (reply == NULL) {
		close(fd);
		return -1;
	}

	snprintf(reply, 4096, "150 Opening binary mode data connection for \"%s\".\r\n", path);
	send(session->fd, reply, strlen(reply), 0);

	int result = 0;
	int timeout = 3000;
	while (1) {
		int recv_bytes =
			stor_cmd_receive(session->port_pasv_fd, (uint8_t *)reply, 4096, timeout);
		if (recv_bytes < 0) {
			result = -1;
			break;
		}
		if (recv_bytes == 0) {
			break;
		}
		if (write(fd, reply, recv_bytes) != recv_bytes) {
			result = -1;
			break;
		}
		fsync(fd);

		timeout = 3000;
	}

	free(reply);
	close(fd);
	close(session->port_pasv_fd);
	session->port_pasv_fd = -1;
	if (session->pasvs_fd > 0) {
		close(session->pasvs_fd);
		session->pasvs_fd = -1;
	}

	if (result != 0) {
		return -1;
	}

	reply = "226 Finished.\r\n";
	send(session->fd, reply, strlen(reply), 0);
	return 0;
}

static int login_cmd_fn(struct ftp_session *session, char *cmd, char *cmd_param)
{
	if (strcmp(cmd_param, USER_NAME) == 0) {
		session->is_anonymous = 0;
		char *reply = "331 Password required.\r\n";
		send(session->fd, reply, strlen(reply), 0);
		session->state = FTP_SESSION_STATE_PASSWD;
	} else if (strcmp(cmd_param, "anonymous") == 0) {
		session->is_anonymous = 1;
		char *reply = "331 anonymous login OK send e-mail address for password.\r\n";
		send(session->fd, reply, strlen(reply), 0);
		session->state = FTP_SESSION_STATE_OK;
		sprintf(session->currentdir, ROOT);
	} else {
		char *reply = "530 Login incorrect. Bye.\r\n";
		send(session->fd, reply, strlen(reply), 0);
		return -1;
	}

	return 0;
}

static int pass_cmd_fn(struct ftp_session *session, char *cmd, char *cmd_param)
{
	char *reply = "230 User logged in\r\n";
	send(session->fd, reply, strlen(reply), 0);
	sprintf(session->currentdir, ROOT);
	return 0;
}

static struct ftp_session_cmd {
	char *cmd;
	int (*cmd_fn)(struct ftp_session *session, char *cmd, char *cmd_param);
} session_cmds[] = {
	{"PORT", port_cmd_fn},
	{"EPRT", port_cmd_fn},
	{"PASV", pasv_cmd_fn},
	{"EPSV", pasv_cmd_fn},
	{"PWD", pwd_cmd_fn},
	{"XPWD", pwd_cmd_fn},
	{"TYPE", type_cmd_fn},
	{"SYST", syst_cmd_fn},
	{"QUIT", quit_cmd_fn},
	{"LIST", list_cmd_fn},
	{"NLST", nlist_cmd_fn},
	{"CWD", cwd_cmd_fn},
	{"CDUP", cdup_cmd_fn},
	{"MKD", mkd_cmd_fn},
	{"RMD", rmd_cmd_fn},
	{"DELE", dele_cmd_fn},
	{"SIZE", size_cmd_fn},
	{"REST", rest_cmd_fn},
	{"RETR", retr_cmd_fn},
	{"STOR", stor_cmd_fn},
	{"USER", login_cmd_fn},
	{"PASS", pass_cmd_fn},
};

int ftp_session_cmd_process(struct ftp_session *session, char *cmd, char *cmd_param)
{
	int array_cnt = sizeof(session_cmds) / sizeof(session_cmds[0]);
	struct ftp_session_cmd *session_cmd = NULL;

	for (int i = 0; i < array_cnt; i++) {
		if (strstr(cmd, session_cmds[i].cmd) == cmd) {
			session_cmd = &session_cmds[i];
			break;
		}
	}

	if (session_cmd == NULL) {
		char *reply = "502 Not Implemented.\r\n";
		send(session->fd, reply, strlen(reply), 0);
		return 0;
	}

	int result = session_cmd->cmd_fn(session, cmd, cmd_param);

	return result;
}
