#ifndef __FW_UART_H
#define __FW_UART_H

#include <stdint.h>

#define UART_FRAME_HEAD  0xAA
#define UART_FRAME_TAIL  0x55
#define UART_FRAME_CMD   0x01
#define UART_FRAME_DATA  0x02

void fw_uart_init(void);

#endif
