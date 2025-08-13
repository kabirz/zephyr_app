#ifndef __24C02_H
#define __24C02_H
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>

#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;

#define DEV_GPIO DEVICE_DT_GET(DT_NODELABEL(gpiob))
#define IIC_SCL(v) gpio_pin_set(DEV_GPIO, 8, v)
#define IIC_SDAOUT(v) gpio_pin_set(DEV_GPIO, 9, v)
#define IIC_SDAIN  gpio_pin_get(DEV_GPIO, 9)

void IIC_Init(void);
void IIC_Start(void);
void IIC_Stop(void);
u8 MCU_Wait_Ack(void);
void MCU_Send_Ack(void);
void MCU_NOAck(void);
void IIC_write_OneByte(u8 Senddata);
u8 IIC_Read_OneByte(u8 ack);

u8 AT24C02_ReadByte(u8 ReadAddr);
void AT24C02_WriteByte(u8 WriteAddr, u8 DataToWrite);

u32 Buf_4Byte(u8 *pBuffer, u32 Date_4Byte, u8 Byte_num, u8 mode);

void AT24C02_Write(u8 WriteAddr, u8 *pBuffer, u8 WriteNum);
void AT24C02_Read(u8 ReadAddr, u8 *pBuffer, u8 ReadNum);

u8 AT24C02_Test(void);
void AT24C02_Init(void);

#endif
