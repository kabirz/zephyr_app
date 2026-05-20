/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * ADC 操纵杆角度采集 + 电源电压采样
 * 基于 Zephyr ADC API, 500ms 周期采集 X/Y 角度, 2s 周期采集电压
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/drivers/adc.h>
#include <common.h>
#include <display.h>
#include <mod-can.h>
#include <mod-gpio.h>
#include <lora.h>

LOG_MODULE_REGISTER(adc_reader, LOG_LEVEL_INF);

#define CAN_SLEEP_MS  20
#define LORA_SLEEP_MS 200
#define DT_SPEC_AND_COMMA(node_id, prop, idx) ADC_DT_SPEC_GET_BY_IDX(node_id, idx),

static const struct adc_dt_spec adc_channels[] = {
	DT_FOREACH_PROP_ELEM(DT_PATH(zephyr_user), io_channels, DT_SPEC_AND_COMMA)};


int adc_init_channels(void)
{
	int ret;

	for (size_t i = 0U; i < ARRAY_SIZE(adc_channels); i++) {
		if (!device_is_ready(adc_channels[i].dev)) {
			LOG_ERR("ADC device not ready");
			return -ENODEV;
		}

		ret = adc_channel_setup_dt(&adc_channels[i]);
		if (ret < 0) {
			LOG_ERR("Could not setup channel #%d (%d)", adc_channels[i].channel_id,
				ret);
			return 0;
		}
	}

	LOG_INF("ADC channels configured successfully");

	return ret;
}

void adc_read_thread(void)
{
	int ret;
	uint16_t buf;
	struct adc_sequence sequence = {.buffer = &buf, .buffer_size = sizeof(buf)};
	int32_t val_mv;
	int x_degree = 0, y_degree = 0;
	uint32_t sleep_ms = global_params.connect_type == CAN_TYPE ? CAN_SLEEP_MS : LORA_SLEEP_MS;
	bool need_send = false;
	uint32_t adc_count = 0;

	LOG_INF("ADC read thread started");

	while (true) {
		if (global_params.sleeping) {
			k_event_wait(&global_params.event, WAKE_EVENT, false, K_FOREVER);
			continue;
		}

		uint32_t t1 = k_uptime_get_32();

		for (size_t i = 0U; i < ARRAY_SIZE(adc_channels) - 1; i++) {
			(void)adc_sequence_init_dt(&adc_channels[i], &sequence);
			ret = adc_read_dt(&adc_channels[i], &sequence);
			if (ret < 0) {
				LOG_ERR("Could not read (%d)", ret);
				continue;
			}
			val_mv = (int32_t)buf;

			ret = adc_raw_to_millivolts_dt(&adc_channels[i], &val_mv);
			if (ret < 0) {
				LOG_ERR(" (value in mV not available)");
			} else {
				LOG_DBG("val[%d]: %d mv", i, val_mv);
			}
			// 500~4500mV → -20~+20 (1° 单位)
			if (i == 0) {
				x_degree = (CLAMP(val_mv * 10 / 6, 500, 4500) - 500) * 40 / 4000 - 20;
			} else {
				y_degree = (CLAMP(val_mv * 10 / 6, 500, 4500) - 500) * 40 / 4000 - 20;
			}
		}

		if (x_degree != global_params.x_degree || y_degree != global_params.y_degree) {
			last_activity_time = k_uptime_get_32();
			global_params.x_degree = x_degree;
			global_params.y_degree = y_degree;
			need_send = true;
		}
		if (need_send || adc_count >= 10) {
			if (global_params.connect_type == CAN_TYPE) {
				mod_can_send_handler_state(&global_params);
			} else {
				if (!global_params.test_mode) {
					lora_send_telemetry(&global_params);
				}
			}
			adc_count = 0;
			need_send = false;
		} else {
			adc_count += 1;
		}
		uint32_t diff = k_uptime_get_32() - t1;
		if (diff < sleep_ms) {
			k_sleep(K_MSEC(sleep_ms - diff));
		}
	}
}
K_THREAD_DEFINE(thread_adc, 1024, adc_read_thread, NULL, NULL, NULL, 7, 0, 0);

void adc_power_thread(void)
{
	int ret;
	uint16_t buf;
	struct adc_sequence sequence = {.buffer = &buf, .buffer_size = sizeof(buf)};
	int32_t val_mv, power_mv = 0;
	static const struct adc_dt_spec *adc_voltage_channel = &adc_channels[ARRAY_SIZE(adc_channels) - 1];
	#define POWER_SAMPLE_COUNT 10
	static int32_t power_samples[POWER_SAMPLE_COUNT];
	static int power_sample_idx = 0;


	LOG_INF("ADC power thread started");
	(void)adc_sequence_init_dt(adc_voltage_channel, &sequence);

	while (true) {
		if (global_params.sleeping) {
			k_event_wait(&global_params.event, WAKE_EVENT, false, K_FOREVER);
			continue;
		}

		ret = adc_read_dt(adc_voltage_channel, &sequence);
		if (ret < 0) {
			LOG_ERR("Could not read power channel (%d)", ret);
		} else {
			val_mv = (int32_t)buf;
			ret = adc_raw_to_millivolts_dt(adc_voltage_channel, &val_mv);
			if (ret == 0) {
				power_samples[power_sample_idx] = val_mv * 2;
				power_sample_idx++;
			}
		}

		if (power_sample_idx >= POWER_SAMPLE_COUNT) {
			int32_t min_v = power_samples[0];
			int32_t max_v = power_samples[0];
			int32_t sum = 0;
			for (int j = 0; j < POWER_SAMPLE_COUNT; j++) {
				if (power_samples[j] < min_v) {
					min_v = power_samples[j];
				}
				if (power_samples[j] > max_v) {
					max_v = power_samples[j];
				}
				sum += power_samples[j];
			}
			power_mv = (sum - min_v - max_v) / (POWER_SAMPLE_COUNT - 2);
			power_sample_idx = 0;

			battery_status_t battery_status = read_battery_status();
			if (power_mv != global_params.power_mv ||
			    global_params.battery_status != battery_status) {
				mod_display_battery(power_mv, battery_status);
				global_params.power_mv = power_mv;
				global_params.battery_status = battery_status;
			}
		}

		k_sleep(K_MSEC(200));
	}
}

K_THREAD_DEFINE(thread_adc_power, 1024, adc_power_thread, NULL, NULL, NULL, 8, 0, 0);
