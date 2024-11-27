#ifndef __MODBUS_INIT_H_
#define __MODBUS_INIT_H_

#define USER_NODE DT_PATH(zephyr_user)

enum {
    INPUT_VER_IDX,         /* FW Version */
    INPUT_AI0_IDX,         /* AI0 data */
    INPUT_AI1_IDX,         /* AI1 data */
    INPUT_AI2_IDX,         /* AI2 data */
    INPUT_AI3_IDX,         /* AI3 data */
    INPUT_DI_IDX,          /* DI bit(0~15) */
    INPUT_MAX_IDX,
};

typedef enum {
    HOLDING_DO_IDX,        /* DO bit(0~7) */
    HOLDING_DI_EN_IDX,     /* DI enable bit(0~15) */
    HOLDING_AI_EN_IDX,     /* AI enable bit(0~3) */
    HOLDING_DI_SI_IDX,     /* DI sampling interval, ms */
    HOLDING_AI_SI_IDX,     /* AI sampling interval, ms */
    HOLDING_HIS_SAVE_IDX,  /* save history to flash, disable: 0, enable: others */
    HOLDING_CAN_ID_IDX,    /* CAN address configure */
    HOLDING_CAN_BPS_IDX,   /* CAN bps */
    HOLDING_RS485_BPS_IDX, /* RS485 uart bps */
    HOLDING_SLAVE_ID_IDX,  /* modbus rtu slave id */
    HOLDING_IP_ADDR_1_IDX, /* ip address aa <aa.bb.cc.dd> */
    HOLDING_IP_ADDR_2_IDX, /* ip address bb <aa.bb.cc.dd> */
    HOLDING_IP_ADDR_3_IDX, /* ip address cc <aa.bb.cc.dd> */
    HOLDING_IP_ADDR_4_IDX, /* ip address dd <aa.bb.cc.dd> */
    HOLDING_TIMESTAMPH_IDX, /* timestamp high 16 bit */
    HOLDING_TIMESTAMPL_IDX, /* timestamp low 16 bit */
    HOLDING_CFG_SAVE_IDX,  /* save configure to flash, disable: 0, enable: others */
    HOLDING_REBOOT_IDX,    /* reboot when set 1 */
    HOLDING_MAX_IDX,
} holding_idx;

#include <zephyr/kernel.h>

#define DI_TYPE   1
#define AI_TYPE   2
#define AI_NUM    4
struct his_data {
    uint16_t type;
    uint32_t timestamps;
    union {
        struct {
            uint16_t di_en_status;
            uint16_t di_value;
        } di __packed;
        struct {
            uint16_t ai_en_status;
            uint16_t ai_value[AI_NUM];
        } ai __packed;
    } __packed;
} __packed;


extern struct modbus_user_callbacks mbs_cbs;
int mb_set_do(uint16_t val);
int update_input_reg(uint16_t addr, uint16_t reg);
int update_holding_reg(uint16_t addr, uint16_t reg);
uint16_t get_holding_reg(uint16_t addr);
uint16_t get_input_reg(size_t index);
int write_history_data(void *data, size_t size);
void history_enable_write(bool enable);
void set_timestamp(time_t t);

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(modbus_main, CONFIG_MODBUS_APP_LOG_LEVEL);
#endif

