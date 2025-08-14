#include <stdio.h>
#include <inttypes.h>

#include <zephyr/kernel.h>
#include <zephyr/net/tls_credentials.h>
#include <zephyr/net/http/server.h>
#include <zephyr/net/http/service.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#include "zephyr/device.h"
#include "zephyr/sys/util.h"
#include <zephyr/drivers/led.h>
#include <zephyr/data/json.h>
#include <zephyr/sys/util_macro.h>
#include <zephyr/posix/time.h>
#include <zephyr/app_version.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(net_http_server_app, LOG_LEVEL_INF);

static uint8_t http_buf[2048];

static const uint8_t index_html_gz[] = {
#include "index.html.gz.inc"
};

static struct http_resource_detail_static index_html_gz_resource_detail = {
	.common =
		{
			.type = HTTP_RESOURCE_TYPE_STATIC,
			.bitmask_of_supported_http_methods = BIT(HTTP_GET),
			.content_encoding = "gzip",
			.content_type = "text/html",
		},
	.static_data = index_html_gz,
	.static_data_len = sizeof(index_html_gz),
};

#define SLOT1_PARTITION      slot1_partition
#define SLOT1_PARTITION_ID   FIXED_PARTITION_ID(SLOT1_PARTITION)
#define SLOT1_PARTITION_DEV  FIXED_PARTITION_DEVICE(SLOT1_PARTITION)
#define SLOT1_PARTITION_NODE DT_NODELABEL(SLOT1_PARTITION)
static struct fw_data {
	bool found;
	bool had_first;
	uint8_t first_line[64];
	size_t offset;
	const struct flash_area *fa;
} fw_d;
#define FLASH_SUCCESS "{\"status\": \"success\"}\r\n"
#define FLASH_FAILED  "{\"status\": \"failed\"}\r\n"
static int fw_upgrade_handler(struct http_client_ctx *client, enum http_data_status status,
					  const struct http_request_ctx *request_ctx,
					  struct http_response_ctx *response_ctx, void *user_data)
{
	struct fw_data *f_data = user_data;

	__ASSERT_NO_MSG(request_ctx->data!= NULL);

	if (status == HTTP_SERVER_DATA_ABORTED) {
		LOG_DBG("Transaction aborted after %zd bytes.", f_data->offset);
		memset(f_data, 0, sizeof(struct fw_data));
		return 0;
	}
	if (!f_data->found) {
		uint8_t *data;
		if ((!f_data->had_first) && strstr(request_ctx->data, "\r\n")) {
			request_ctx->data[request_ctx->data_len] = '\0';
			memcpy(f_data->first_line, request_ctx->data,
			       MIN((uint8_t *)strstr(request_ctx->data, "\r\n") - request_ctx->data,
				   sizeof(f_data->first_line)));
			f_data->had_first = true;
		}
		data = strstr(request_ctx->data, "\r\n\r\n");
		if (data) {
			data += 4;
			if (flash_area_open(SLOT1_PARTITION_ID, &f_data->fa)) {
				LOG_ERR("flash area open failed");
				memset(f_data, 0, sizeof(struct fw_data));
				return -1;
			}
			flash_area_erase(f_data->fa, 0, f_data->fa->fa_size);
			LOG_INF("starting upgrade firmware, size: %d", f_data->fa->fa_size);
			if (flash_area_write(f_data->fa, f_data->offset, data,
					     request_ctx->data_len - (data - request_ctx->data))) {
				memset(f_data, 0, sizeof(struct fw_data));
				LOG_ERR("flash area write failed");
				return -1;
			}
			f_data->found = true;
			f_data->offset = request_ctx->data_len - (data - request_ctx->data);
		}
	} else {
		int _len = f_data->offset + request_ctx->data_len - f_data->fa->fa_size;
		int len = 0;
		if (_len > 0) {
			uint8_t *off = (uint8_t *)strstr(request_ctx->data+ request_ctx->data_len - _len, f_data->first_line);
			if (off) {
				len = off - request_ctx->data - 2;
			} else {
				LOG_ERR("file not right");
			}
		}
		if (flash_area_write(f_data->fa, f_data->offset, request_ctx->data, request_ctx->data_len)) {
			LOG_ERR("flash area write failed");
			memset(f_data, 0, sizeof(struct fw_data));
			return -1;
		}
		f_data->offset += len;
	}
	if (status == HTTP_SERVER_DATA_FINAL) {
		if (f_data->offset != f_data->fa->fa_size) {
			LOG_ERR("write error, %x, %x", f_data->offset, f_data->fa->fa_size);
			response_ctx->body = FLASH_FAILED;
			response_ctx->body_len = strlen(FLASH_FAILED);
			response_ctx->status = HTTP_400_BAD_REQUEST;
		} else {
			LOG_INF("write finished, total size: %d", f_data->offset);
			response_ctx->body = FLASH_SUCCESS;
			response_ctx->body_len = strlen(FLASH_SUCCESS);
			extern void set_reboot_status(bool val);
			set_reboot_status(true);
		}
		response_ctx->final_chunk = (status == HTTP_SERVER_DATA_FINAL);
		memset(f_data, 0, sizeof(struct fw_data));
	}

	return 0;
}

static struct http_resource_detail_dynamic fw_upgrade_resource_detail = {
	.common =
		{
			.type = HTTP_RESOURCE_TYPE_DYNAMIC,
			.bitmask_of_supported_http_methods = BIT(HTTP_GET) | BIT(HTTP_POST),
			.content_type = "application/json",
		},
	.cb = fw_upgrade_handler,
	.user_data = &fw_d,
};

static int get_version_handler(struct http_client_ctx *client, enum http_data_status status,
			       const struct http_request_ctx *request_ctx,
			       struct http_response_ctx *response_ctx, void *user_data)
{
	int ret;
	static uint8_t ver_buf[64];

	if (status == HTTP_SERVER_DATA_FINAL) {
		ret = snprintf(ver_buf, sizeof(ver_buf), "build time: %s-%s, version: %s", __DATE__,
			       __TIME__, APP_VERSION_TWEAK_STRING);
		if (ret < 0) {
			LOG_ERR("Failed to snprintf uptime, err %d", ret);
			return ret;
		}

		response_ctx->body = ver_buf;
		response_ctx->body_len = ret;
		response_ctx->final_chunk = true;
	}

	return 0;
}

static struct http_resource_detail_dynamic get_version_resource_detail = {
	.common =
		{
			.type = HTTP_RESOURCE_TYPE_DYNAMIC,
			.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		},
	.cb = get_version_handler,
	.user_data = NULL,
};

#if defined(CONFIG_FILE_SYSTEM)
#if DT_NODE_EXISTS(DT_NODELABEL(lfs1))
#define ROOT DT_PROP(DT_NODELABEL(lfs1), mount_point)
#elif DT_NODE_EXISTS(DT_INST(0, zephyr_flash_disk))
#define ROOT "/" DT_PROP(DT_INST(0, zephyr_flash_disk), disk_name) ":"
#else
#error "Must enable filesystem"
#endif

static struct http_resource_detail_static_fs fs_resource_detail = {
	.common =
		{
			.type = HTTP_RESOURCE_TYPE_STATIC_FS,
			.bitmask_of_supported_http_methods = BIT(HTTP_GET),
		},
	.fs_path = "",
};

#include <zephyr/posix/dirent.h>
#include <zephyr/posix/fcntl.h>
#include <zephyr/posix/unistd.h>

static int files_handler(struct http_client_ctx *client, enum http_data_status status,
			 const struct http_request_ctx *request_ctx,
			 struct http_response_ctx *response_ctx, void *user_data)
{
	uint8_t buf[128];

	LOG_DBG("Uptime handler status %d", status);

	/* A payload is not expected with the GET request. Ignore any data and wait until
	 * final callback before sending response
	 */
	if (status == HTTP_SERVER_DATA_FINAL) {
		DIR *dir;
		struct stat st;
		struct dirent *ptr;
		int offset = 0;

		offset += snprintf(http_buf, 2048, "[ ");
		dir = opendir(ROOT);
		if (dir == NULL) {
			LOG_ERR("ROOT is not found");
			goto end_opendir;
		}

		while ((ptr = readdir(dir)) != NULL) {
			if (snprintf(buf, 128, ROOT "/%s", ptr->d_name) < 0) {
				continue;
			}
			if (stat(buf, &st) == 0) {
				if (!S_ISREG(st.st_mode)) {
					continue;
				}
				offset += snprintf(http_buf + offset, 2048 - offset,
						   "{\"name\": \"%s\", \"size\": %ld},", buf,
						   st.st_size);
			}
		}
end_opendir:
		http_buf[offset - 1] = ']';
		closedir(dir);

		response_ctx->body = http_buf;
		response_ctx->body_len = offset;
		response_ctx->final_chunk = true;
		return 0;
	}
	response_ctx->body = NULL;
	response_ctx->body_len = 0;
	response_ctx->final_chunk = false;
	return 0;
}

static struct http_resource_detail_dynamic filelists_resource_detail = {
	.common =
		{
			.type = HTTP_RESOURCE_TYPE_DYNAMIC,
			.bitmask_of_supported_http_methods = BIT(HTTP_GET),
			.content_type = "application/json",
		},
	.cb = files_handler,
	.user_data = NULL,
};
#endif

static uint16_t test_http_service_port = 80;
HTTP_SERVICE_DEFINE(test_http_service, NULL, &test_http_service_port, 1, 10, NULL, NULL);

HTTP_RESOURCE_DEFINE(index_html_gz_resource, test_http_service, "/",
		     &index_html_gz_resource_detail);
HTTP_RESOURCE_DEFINE(fw_resource, test_http_service, "/fw_upgrade", &fw_upgrade_resource_detail);
HTTP_RESOURCE_DEFINE(uptime_resource, test_http_service, "/version", &get_version_resource_detail);

#if defined(CONFIG_FILE_SYSTEM)
HTTP_SERVER_CONTENT_TYPE(raw, "application/octet-stream");
HTTP_RESOURCE_DEFINE(fs_resource, test_http_service, ROOT "/*", &fs_resource_detail);
HTTP_RESOURCE_DEFINE(file_resource, test_http_service, "/filelists", &filelists_resource_detail);
#endif

int init_http_server(void)
{
	return http_server_start();
}

SYS_INIT(init_http_server, APPLICATION, 15);
