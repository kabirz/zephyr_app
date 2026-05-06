/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * ADC 操纵杆角度采集 + 电源电压采样
 * 基于 Zephyr ADC API, 500ms 周期采集 X/Y 角度, 5s 周期采集电压
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

#define DT_SPEC_AND_COMMA(node_id, prop, idx) ADC_DT_SPEC_GET_BY_IDX(node_id, idx),

static const struct adc_dt_spec adc_channels[] = {
	DT_FOREACH_PROP_ELEM(DT_PATH(zephyr_user), io_channels, DT_SPEC_AND_COMMA)};

static int adc_init_channels(void)
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
	int32_t val_mv, power_mv = 0;
	uint32_t lora_send_count = 0;
	int x_degree = 0, y_degree = 0;

	#define POWER_SAMPLE_COUNT 10
	int32_t power_samples[POWER_SAMPLE_COUNT];
	int power_sample_idx = 0;

	LOG_INF("ADC read thread started");

	ret = adc_init_channels();
	if (ret < 0) {
		LOG_ERR("ADC initialization failed");
		return;
	}

	while (true) {
		if (global_params.sleeping) {
			k_event_wait(&global_params.event, WAKE_EVENT, false, K_FOREVER);
			continue;
		}

		uint32_t t1 = k_uptime_get_32();
		for (size_t i = 0U; i < ARRAY_SIZE(adc_channels); i++) {
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
			switch (i) {
			case 0:
				// X: 500~4500mV → -200~+200 (0.1° 单位)
				x_degree = (CLAMP(val_mv * 10 / 6, 500, 4500) - 500) * 400 / 4000 - 200;
				break;
			case 1:
				// Y: 500~4500mV → -200~+200 (0.1° 单位)
				y_degree = (CLAMP(val_mv * 10 / 6, 500, 4500) - 500) * 400 / 4000 - 200;
				break;
			case 2:
				// Power VCC: 存入采样缓冲区
				power_samples[power_sample_idx] = val_mv * 2;
				power_sample_idx++;
				break;
			default:
				break;
			}
		}

		if (x_degree != global_params.x_degree || y_degree != global_params.y_degree) {
			last_activity_time = k_uptime_get_32();
			mod_display_handler_xy(x_degree, y_degree);
			global_params.x_degree = x_degree;
			global_params.y_degree = y_degree;
			if (global_params.connect_type == CAN_TYPE) {
				mod_can_send_handler_state(&global_params);
			} else {
				if (!lora_is_test_mode()) {
					lora_send_telemetry(&global_params);
					lora_send_count = 0;
				}
			}
		} else if(lora_send_count < 10) {
			if (global_params.connect_type == LORA_TYPE &&
			    !lora_is_test_mode()) {
				lora_send_telemetry(&global_params);
				lora_send_count += 1;
			}

		}

		/* 每 10 次采样 (2s) 去极值取平均更新电池电量 */
		if (power_sample_idx >= POWER_SAMPLE_COUNT) {
			int32_t min_v = power_samples[0];
			int32_t max_v = power_samples[0];
			int32_t sum = 0;
			for (int j = 0; j < POWER_SAMPLE_COUNT; j++) {
				if (power_samples[j] < min_v)
					min_v = power_samples[j];
				if (power_samples[j] > max_v)
					max_v = power_samples[j];
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

		uint32_t diff = k_uptime_get_32() - t1;
		if (diff < 200)
			k_sleep(K_MSEC(200 - diff));
	}
}

K_THREAD_DEFINE(adc_thread_id, 1024, adc_read_thread, NULL, NULL, NULL, 7, 0, 0);
