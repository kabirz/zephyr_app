#include <zephyr/kernel.h>
#include <zephyr/drivers/rtc.h>
#include <zephyr/posix/time.h>
#include <zephyr/logging/log_ctrl.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(time, LOG_LEVEL_INF);
static const struct device *rtc_dev = DEVICE_DT_GET(DT_NODELABEL(rtc));

#ifdef CONFIG_LOG
static log_timestamp_t sync_rtc_timestamp_get(void)
{
	return (log_timestamp_t)time(NULL);
}
#endif

void set_timestamp(time_t t)
{
	struct rtc_time *rt = (struct rtc_time *)gmtime(&t);
	struct timespec ts;

	rtc_set_time(rtc_dev, rt);

	time_t time_set = mktime((struct tm *)rt);

	ts.tv_sec = time_set;
	ts.tv_nsec = 0;

	if (clock_settime(CLOCK_REALTIME, &ts) < 0) {
		LOG_ERR("Failed to set clock time");
		return;
	}
}

int clock_init(void)
{
	struct timespec ts;
	struct rtc_time rt;

	rtc_get_time(rtc_dev, &rt);

	time_t time_set = mktime((struct tm *)&rt);

	ts.tv_sec = time_set;
	ts.tv_nsec = 0;

	if (clock_settime(CLOCK_REALTIME, &ts) < 0) {
		LOG_ERR("Failed to set clock time");
		return -1;
	}
#ifdef CONFIG_LOG
	log_set_timestamp_func(sync_rtc_timestamp_get, 1); // sys_clock_hw_cycles_per_sec());
#endif
	return 0;
}

SYS_INIT(clock_init, POST_KERNEL, 41);
