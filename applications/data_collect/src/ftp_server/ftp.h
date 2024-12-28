#ifndef __FTP_S_H__
#define __FTP_S_H__

#include <stdint.h>
#include <stdbool.h>
#include <zephyr/net/socket.h>

#if DT_NODE_EXISTS(DT_NODELABEL(lfs1))
#define ROOT DT_PROP(DT_NODELABEL(lfs1), mount_point)
#elif DT_NODE_EXISTS(DT_INST(0, zephyr_flash_disk))
#define ROOT "/" DT_PROP(DT_INST(0, zephyr_flash_disk), disk_name) ":"
#else
#error "Must enable filesystem"
#endif

#define USER_NAME           "admin"
#define USER_PAWD           "admin"
#define FTP_SESSION_TIMEOUT (120 * 1000)

enum ftp_session_state {
	FTP_SESSION_STATE_USER = 0,
	FTP_SESSION_STATE_PASSWD,
	FTP_SESSION_STATE_OK,
};

struct ftp_session {
	int fd;
	int is_anonymous;
	int port_pasv_fd;
	int pasvs_fd;
	int offset;
	int64_t time;
	enum ftp_session_state state;
	char currentdir[256];
};

extern int ftp_proccess(struct ftp_session *session, char *buf);
extern int ftp_session_cmd_process(struct ftp_session *session, char *cmd, char *cmd_param);

#endif
