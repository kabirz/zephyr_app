#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/drivers/adc.h>
#include <common.h>
#include <display.h>
#include <mod-can.h>
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
	int32_t val_mv, power_level = 0;
	int x_degree = 0, y_degree = 0;
	int adc_times = 0;

	LOG_INF("ADC read thread started");

	ret = adc_init_channels();
	if (ret < 0) {
		LOG_ERR("ADC initialization failed");
		return;
	}

	while (true) {
		uint32_t t1 = k_uptime_get_32();
		adc_times++;
		for (size_t i = 0U; i < ARRAY_SIZE(adc_channels); i++) {
			// 电量采集间隔: 每 10 个 ADC 周期 (5s)
			if (i == 2) {
				if (adc_times < 10) {
					continue;
				}
				adc_times = 0;
			}
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
				// Power VCC → 电量百分比 (3.0V~4.2V 线性映射)
				power_level = (uint8_t)CLAMP((val_mv - 3000) * 100 / 1200, 0, 100);
				break;
			default:
				break;
			}
		}

		if (x_degree != global_params.x_degree || y_degree != global_params.y_degree) {
			mod_display_handler_xy(x_degree, y_degree);
			global_params.x_degree = x_degree;
			global_params.y_degree = y_degree;
			if (global_params.connect_type == CAN_TYPE) {
				mod_can_send_handler_state(&global_params);
			} else {
				lora_send_telemetry(&global_params);
			}
		}

		if (power_level != global_params.power_level) {
			mod_display_battery(power_level);
			global_params.power_level = power_level;
		}

		uint32_t diff = k_uptime_get_32() - t1;
		k_sleep(K_MSEC(500 - diff));
	}
}

K_THREAD_DEFINE(adc_thread_id, 1024, adc_read_thread, NULL, NULL, NULL, 7, 0, 0);
