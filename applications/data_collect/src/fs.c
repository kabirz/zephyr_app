#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(app_mount, LOG_LEVEL_INF);
#include <ff.h>

#ifdef CONFIG_FS_FATFS_HAS_RTC
#include <zephyr/posix/time.h>
DWORD get_fattime(void)
{
	time_t unix_time = time(NULL);
	struct tm *cal;

	/* Convert to calendar time */
	cal = localtime(&unix_time);

	/* From http://elm-chan.org/fsw/ff/doc/fattime.html */
	return (DWORD)(cal->tm_year - 80) << 25 | (DWORD)(cal->tm_mon + 1) << 21 |
	       (DWORD)cal->tm_mday << 16 | (DWORD)cal->tm_hour << 11 | (DWORD)cal->tm_min << 5 |
	       (DWORD)cal->tm_sec >> 1;
}
#endif

static FATFS _fat_fs;
/* mounting info */
static struct fs_mount_t fatfs_mnt = {
	.type = FS_FATFS,
	.fs_data = &_fat_fs,
	.mnt_point = "/" DT_PROP(DT_INST(0, zephyr_flash_disk), disk_name) ":",
};
static int _fs_mount_init(void)
{
	if (fs_mount(&fatfs_mnt)) {
		LOG_ERR("fs mount failed");
		return -1;
	}
	return 0;
}
SYS_INIT(_fs_mount_init, APPLICATION, 90);
