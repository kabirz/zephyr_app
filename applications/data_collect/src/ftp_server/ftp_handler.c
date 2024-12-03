#include "ftp.h"
#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(ftp, LOG_LEVEL_INF);

int ftp_proccess(struct ftp_session *session, char *buf)
{
	int result;
	char *cmd = buf;
	char *cmd_param = strchr(cmd, ' ');

	if (cmd_param) {
		*cmd_param = '\0';
		cmd_param++;
	}

	switch (session->state) {
	case FTP_SESSION_STATE_USER: {
		if (strstr(cmd, "USER") != cmd) {
			char *reply = "502 Not Implemented.\r\n";
			send(session->fd, reply, strlen(reply), 0);
			break;
		}

		if (strcmp(cmd_param, "anonymous") == 0) {
			session->is_anonymous = 1;
			char *reply =
				"331 anonymous login OK send e-mail address for password.\r\n";
			send(session->fd, reply, strlen(reply), 0);
			session->state = FTP_SESSION_STATE_OK;
			break;
		}

		if (strcmp(cmd_param, USER_NAME) == 0) {
			session->is_anonymous = 0;
			char *reply = "331 Password required.\r\n";
			send(session->fd, reply, strlen(reply), 0);
			session->state = FTP_SESSION_STATE_PASSWD;
			break;
		}

		char *reply = "530 Login incorrect. Bye.\r\n";
		send(session->fd, reply, strlen(reply), 0);
		result = -1;
		break;
	}

	case FTP_SESSION_STATE_PASSWD: {
		if (strstr(cmd, "PASS") != cmd) {
			char *reply = "502 Not Implemented.\r\n";
			send(session->fd, reply, strlen(reply), 0);
			break;
		}

		if (session->is_anonymous || (strcmp(cmd_param, USER_PAWD) == 0)) {
			char *reply = "230 User logged in\r\n";
			send(session->fd, reply, strlen(reply), 0);
			memset(session->currentdir, 0, sizeof(session->currentdir));
			sprintf(session->currentdir, ROOT);
			session->state = FTP_SESSION_STATE_OK;
			break;
		}

		char *reply = "530 Login incorrect. Bye.\r\n";
		send(session->fd, reply, strlen(reply), 0);
		result = -1;
		break;
	}

	case FTP_SESSION_STATE_OK: {
		int rc = ftp_session_cmd_process(session, cmd, cmd_param);
		if (rc) {
			result = -1;
			break;
		}
		break;
	}

	default:
		result = -1;
		break;
	}

	return 0;
}
