#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/drivers/adc.h>

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
	int32_t val_mv, mv_vcc;
	float x_degree, y_degree;

	LOG_INF("ADC read thread started");

	ret = adc_init_channels();
	if (ret < 0) {
		LOG_ERR("ADC initialization failed");
		return;
	}

	while (1) {
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
				// X
				x_degree = (float)(CLAMP(val_mv * 10 / 6, 500, 4500) - 500)/(4500-500)*40-20;
				break;
			case 1:
				// Y
				y_degree = (float)(CLAMP(val_mv * 10 / 6, 500, 4500) - 500)/(4500-500)*40-20;
				break;
			case 2:
				// Power VCC
				mv_vcc = val_mv;
				break;
			default:
				break;
			}
			k_msleep(1);
		}
		k_msleep(30);
	}
}

K_THREAD_DEFINE(adc_thread_id, 2048, adc_read_thread, NULL, NULL, NULL, 7, 0, 0);
