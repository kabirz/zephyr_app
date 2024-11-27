#include "init.h"
#include <zephyr/drivers/adc.h>
#include <zephyr/posix/time.h>

#define DT_SPEC_AND_COMMA(node_id, prop, idx) ADC_DT_SPEC_GET_BY_IDX(node_id, idx),

static const struct adc_dt_spec adc_channels[] = {
    DT_FOREACH_PROP_ELEM(USER_NODE, io_channels, DT_SPEC_AND_COMMA)
};

#if DT_PROP_LEN(USER_NODE, io_channels) > 4 
#error "Max adc channel number is 4"
#endif

int adc_handler(void)
{
    int err;
    uint16_t buf;
    struct his_data data;
    struct adc_sequence sequence = {.buffer = &buf, .buffer_size = sizeof(buf)};

    for (size_t i = 0U; i < ARRAY_SIZE(adc_channels); i++) {
	    if (!adc_is_ready_dt(&adc_channels[i])) {
	        LOG_ERR("ADC controller device %s not ready", adc_channels[i].dev->name);
	        return 0;
	    }

	    err = adc_channel_setup_dt(&adc_channels[i]);
	    if (err < 0) {
	        LOG_ERR("Could not setup channel #%d (%d)", i, err);
	        return 0;
	    }
    }

    while (1) {
	    k_msleep(get_holding_reg(HOLDING_AI_SI_IDX));

	    for (size_t i = 0U; i < ARRAY_SIZE(adc_channels); i++) {
	        if (get_holding_reg(HOLDING_AI_EN_IDX) & (1 << i)) {
	            int32_t val_mv;

	            (void)adc_sequence_init_dt(&adc_channels[i], &sequence);

	            err = adc_read_dt(&adc_channels[i], &sequence);
	            if (err < 0) {
		            LOG_ERR("Could not read (%d)", err);
		            continue;
	            }

	            if (adc_channels[i].channel_cfg.differential) {
		            val_mv = (int32_t)((int16_t)buf);
	            } else {
		            val_mv = (int32_t)buf;
	            }
	            err = adc_raw_to_millivolts_dt(&adc_channels[i], &val_mv);
	            if (err < 0) {
		            LOG_ERR(" (value in mV not available)");
	            } else {
#ifdef CONFIG_BOARD_DAQ_F407VET6
		            if (i < 2) {
		                val_mv = (int)(7.414 * val_mv / 10);
		                LOG_DBG("val[%d]: %d.%d mA", i, val_mv / 100, val_mv % 100);
		            } else {
		                val_mv = (int)(3.7037 * val_mv / 10);
		                LOG_DBG("val[%d]: %d.%d mV", i, val_mv / 100, val_mv % 100);
		            }
#else
	    	        LOG_DBG("val[%d]: %d mv", i, val_mv);
#endif
	    	        update_input_reg(INPUT_AI0_IDX+i, val_mv);
	            }
	        } else {
	            update_input_reg(INPUT_AI0_IDX+i, 0);
	        }
	    }
	    if (get_holding_reg(HOLDING_HIS_SAVE_IDX)) {
	        data.type = AI_TYPE;
	        data.timestamps = (uint32_t)time(NULL);
	        for (size_t i = 0; i < ARRAY_SIZE(adc_channels); i++) {
	            data.ai.ai_value[i] = get_input_reg(INPUT_AI0_IDX + i);
	        }
	        data.ai.ai_en_status = get_holding_reg(HOLDING_AI_EN_IDX);
            write_history_data(&data, sizeof(data));
	    }
    }
    return 0;
}

K_THREAD_DEFINE(adc_io, CONFIG_MODBUS_ADC_STACK_SIZE, adc_handler, NULL, NULL, NULL, CONFIG_MODBUS_ADC_PRIORITY, 0, 0);
