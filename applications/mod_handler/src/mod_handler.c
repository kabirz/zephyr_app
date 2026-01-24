/*
 * Copyright (c) 2026 Kabirz.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <stdlib.h>
#include <string.h>

LOG_MODULE_REGISTER(modhandler, LOG_LEVEL_DBG);

/* ==================== Configuration ==================== */
#define HANDLER_UART_NODE       DT_NODELABEL(usart2)
#define HANDLER_BUTTON_NODE     DT_ALIAS(sw0)

#define DATA_PACKAGE_LEN        18
#define HANDLER_BUFFER_LEN      32
#define CMD_HEAD                '$'
#define CMD_TAIL                '@'

/* ==================== Data Structures ==================== */
typedef struct {
    float x_degree;
    float y_degree;
    float z_degree;
    int btn_state;
    int btn_box;
} HandleState_t;

/* ==================== Private Variables ==================== */
static const struct device *uart_dev;
static const struct gpio_dt_spec button_spec = GPIO_DT_SPEC_GET(HANDLER_BUTTON_NODE, gpios);
static struct gpio_callback button_cb_data;

static uint8_t g_cmd_buf[HANDLER_BUFFER_LEN];
static HandleState_t g_state;
static K_MUTEX_DEFINE(state_mutex);
static bool module_initialized = false;
static bool uart_enabled = false;

/* ==================== Protocol Parsing ==================== */
static inline int hex_to_int(const uint8_t *data, size_t len)
{
    char temp[4] = {0};
    memcpy(temp, data, MIN(len, 3));
    return (int)strtol(temp, NULL, 16);
}

static inline int decode_axis(const uint8_t *buf, char axis_id, float *degree)
{
    if (axis_id != 0 && buf[0] != axis_id) {
        return -EINVAL;
    }

    int value = hex_to_int(&buf[1], 3);
    int sign = value & 0x100;
    value &= 0xFF;
    *degree = sign ? -value : value;

    return 0;
}

static int parse_packet(const uint8_t *buf, size_t len)
{
    if (len != DATA_PACKAGE_LEN + 1) {
        return -EINVAL;
    }

    HandleState_t state;

    if (decode_axis(&buf[1], 0, &state.x_degree) != 0 ||
        decode_axis(&buf[6], '1', &state.y_degree) != 0 ||
        decode_axis(&buf[11], '2', &state.z_degree) != 0) {
        return -EINVAL;
    }

    state.btn_state = (buf[17] == '1') ? 1 : 0;

    k_mutex_lock(&state_mutex, K_FOREVER);
    g_state.x_degree = state.x_degree;
    g_state.y_degree = state.y_degree;
    g_state.z_degree = state.z_degree;
    g_state.btn_state = state.btn_state;
    k_mutex_unlock(&state_mutex);

    return 0;
}

/* ==================== UART Interrupt Handler ==================== */
static void uart_rx_handler(const struct device *dev, void *user_data)
{
    static enum { WAIT_HEAD, COLLECT_DATA } state = WAIT_HEAD;
    static uint32_t frame_len = 0;
    uint8_t byte;

    if (!uart_irq_update(dev) || !uart_irq_rx_ready(dev)) {
        return;
    }

    while (uart_fifo_read(dev, &byte, 1) == 1) {
        switch (state) {
        case WAIT_HEAD:
            if (byte == CMD_HEAD) {
                g_cmd_buf[0] = byte;
                frame_len = 1;
                state = COLLECT_DATA;
            }
            break;

        case COLLECT_DATA:
            if (byte == CMD_TAIL) {
                if (frame_len == DATA_PACKAGE_LEN) {
                    g_cmd_buf[frame_len++] = byte;
                    parse_packet(g_cmd_buf, frame_len);
                }
                state = WAIT_HEAD;
            } else if (byte == CMD_HEAD) {
                state = WAIT_HEAD;
            } else if (frame_len < HANDLER_BUFFER_LEN) {
                g_cmd_buf[frame_len++] = byte;
            } else {
                state = WAIT_HEAD;
            }
            break;
        }
    }
}

/* ==================== GPIO Interrupt Handler ==================== */
static void button_callback(const struct device *dev,
                           struct gpio_callback *cb,
                           uint32_t pins)
{
    int val = gpio_pin_get_dt(&button_spec);
    k_mutex_lock(&state_mutex, K_FOREVER);
    g_state.btn_box = val;
    k_mutex_unlock(&state_mutex);
}

/* ==================== Public API ==================== */
int ModHandler_Init(void)
{
    int ret;

    if (module_initialized) {
        return 0;
    }

    /* Initialize UART */
    uart_dev = DEVICE_DT_GET(HANDLER_UART_NODE);
    if (!device_is_ready(uart_dev)) {
        LOG_ERR("UART device not ready");
        return -ENODEV;
    }

    const struct uart_config cfg = {
        .baudrate = 9600,
        .parity = UART_CFG_PARITY_NONE,
        .stop_bits = UART_CFG_STOP_BITS_1,
        .data_bits = UART_CFG_DATA_BITS_8,
        .flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
    };

    ret = uart_configure(uart_dev, &cfg);
    if (ret) {
        LOG_ERR("UART config failed: %d", ret);
        return ret;
    }

    uart_irq_callback_user_data_set(uart_dev, uart_rx_handler, NULL);

    /* Initialize GPIO button */
    if (!device_is_ready(button_spec.port)) {
        LOG_ERR("Button GPIO not ready");
        return -ENODEV;
    }

    ret = gpio_pin_configure_dt(&button_spec, GPIO_INPUT);
    if (ret) {
        LOG_ERR("GPIO config failed: %d", ret);
        return ret;
    }

    ret = gpio_pin_interrupt_configure_dt(&button_spec, GPIO_INT_EDGE_BOTH);
    if (ret) {
        LOG_ERR("GPIO IRQ config failed: %d", ret);
        return ret;
    }

    gpio_init_callback(&button_cb_data, button_callback, BIT(button_spec.pin));
    gpio_add_callback(button_spec.port, &button_cb_data);

    /* Initialize state */
    memset(&g_state, 0, sizeof(g_state));
    g_state.btn_box = gpio_pin_get_dt(&button_spec);

    module_initialized = true;
    LOG_INF("Handler module initialized");
    return 0;
}

int ModHandler_Start(void)
{
    if (!module_initialized) {
        return -EINVAL;
    }

    if (!uart_enabled) {
        uart_irq_rx_enable(uart_dev);
        uart_enabled = true;
        LOG_INF("Handler started");
    }

    return 0;
}

int ModHandler_Stop(void)
{
    if (!module_initialized) {
        return -EINVAL;
    }

    if (uart_enabled) {
        uart_irq_rx_disable(uart_dev);
        uart_enabled = false;
        LOG_INF("Handler stopped");
    }

    return 0;
}

int ModHandler_GetState(HandleState_t *state)
{
    if (!module_initialized || !state) {
        return -EINVAL;
    }

    k_mutex_lock(&state_mutex, K_FOREVER);
    *state = g_state;
    k_mutex_unlock(&state_mutex);

    return 0;
}

/* ==================== Shell Commands ==================== */
#ifdef CONFIG_SHELL
#include <zephyr/shell/shell.h>

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
    HandleState_t state;

    if (ModHandler_GetState(&state) == 0) {
        shell_print(sh, "Button: %s, Box: %s",
                   state.btn_state ? "pressed" : "released",
                   state.btn_box ? "pressed" : "released");
        shell_print(sh, "X=%d, Y=%d, Z=%d",
                   (int)state.x_degree, (int)state.y_degree, (int)state.z_degree);
        return 0;
    }

    shell_error(sh, "Failed to get status");
    return -1;
}

static int cmd_start(const struct shell *sh, size_t argc, char **argv)
{
    return ModHandler_Start() == 0 ? 0 : -1;
}

static int cmd_stop(const struct shell *sh, size_t argc, char **argv)
{
    return ModHandler_Stop() == 0 ? 0 : -1;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_handler,
    SHELL_CMD(status, NULL, "Show handler status", cmd_status),
    SHELL_CMD(start, NULL, "Start handler", cmd_start),
    SHELL_CMD(stop, NULL, "Stop handler", cmd_stop),
    SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(mhandler, &sub_handler, "Handler commands", NULL);
#endif /* CONFIG_SHELL */
