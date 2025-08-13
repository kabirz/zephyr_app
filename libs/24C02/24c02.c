#include "24c02.h"

void IIC_Init(void)
{
	gpio_pin_configure(DEV_GPIO, 8, GPIO_OUTPUT|GPIO_OPEN_DRAIN);
	gpio_pin_configure(DEV_GPIO, 9, GPIO_INPUT|GPIO_OUTPUT|GPIO_OPEN_DRAIN);
	IIC_SCL(1);
	IIC_SDAOUT(1);
}

void IIC_Start(void)
{
	IIC_SDAOUT(1);
	IIC_SCL(1);
	k_busy_wait(4);
	IIC_SDAOUT(0);
	k_busy_wait(4);
	IIC_SCL(0);
}

void IIC_Stop(void)
{
	IIC_SCL(0);
	IIC_SDAOUT(0);
	k_busy_wait(4);
	IIC_SCL(1);
	k_busy_wait(4);
	IIC_SDAOUT(1);
}

u8 MCU_Wait_Ack(void)
{
	u8 WaitTime = 0;
	IIC_SDAOUT(1);
	k_busy_wait(1);
	IIC_SCL(1);
	k_busy_wait(1);
	while (IIC_SDAIN) {
		WaitTime++;
		if (WaitTime > 250) {
			IIC_Stop();
			return 1;
		}
	}
	IIC_SCL(0);
	return 0;
}

void MCU_Send_Ack(void)
{
	IIC_SCL(0);
	IIC_SDAOUT(0);
	k_busy_wait(2);
	IIC_SCL(1);
	k_busy_wait(2);
	IIC_SCL(0);
}

void MCU_NOAck(void)
{
	IIC_SCL(0);
	IIC_SDAOUT(1);
	k_busy_wait(4);
	IIC_SCL(1);
	k_busy_wait(4);
	IIC_SCL(0);
}

void IIC_write_OneByte(u8 Senddata)
{
	u8 t;
	IIC_SCL(0);
	for (t = 0; t < 8; t++) {
		IIC_SDAOUT((Senddata & 0x80) >> 7);
		Senddata <<= 1;
		k_busy_wait(2);
		IIC_SCL(1);
		k_busy_wait(2);
		IIC_SCL(0);
		k_busy_wait(2);
	}
}

u8 IIC_Read_OneByte(u8 ack)
{
	u8 i, receivedata = 0;
	for (i = 0; i < 8; i++) {
		IIC_SCL(0);
		k_busy_wait(2);
		IIC_SCL(1);
		receivedata <<= 1;
		if (IIC_SDAIN) {
			receivedata++;
		}
		k_busy_wait(1);
	}
	if (!ack) {
		MCU_NOAck();
	} else {
		MCU_Send_Ack();
	}
	return receivedata;
}
void AT24C02_Init(void)
{
	IIC_Init();
}

u8 AT24C02_ReadByte(u8 ReadAddr)
{
	u8 receivedata = 0;

	IIC_Start();
	IIC_write_OneByte(0XA0);
	MCU_Wait_Ack();
	IIC_write_OneByte(ReadAddr);
	MCU_Wait_Ack();
	IIC_Start();
	IIC_write_OneByte(0XA1);
	MCU_Wait_Ack();
	receivedata = IIC_Read_OneByte(0);
	IIC_Stop();

	return receivedata;
}

void AT24C02_WriteByte(u8 WriteAddr, u8 WriteData)
{
	IIC_Start();
	IIC_write_OneByte(0XA0);
	MCU_Wait_Ack();
	IIC_write_OneByte(WriteAddr);
	MCU_Wait_Ack();
	IIC_write_OneByte(WriteData);
	MCU_Wait_Ack();
	IIC_Stop();
	k_msleep(10);
}

u8 AT24C02_Test(void)
{
	u8 Testdata;
	Testdata = AT24C02_ReadByte(255);
	if (Testdata == 0XAB) {
		return 0;
	} else {
		AT24C02_WriteByte(255, 0XAB);
		Testdata = AT24C02_ReadByte(255);
		if (Testdata == 0XAB) {
			return 0;
		}
	}
	return 1;
}

u32 Buf_4Byte(u8 *pBuffer, u32 Date_4Byte, u8 Byte_num, u8 mode)
{
	u8 i;
	u32 middata = 0;
	if (mode) {
		for (i = 0; i < Byte_num; i++) {
			*pBuffer++ = (Date_4Byte >> (8 * i)) & 0xff;
		}
		return 0;
	} else {
		Date_4Byte = 0;
		pBuffer += (Byte_num - 1);
		for (i = 0; i < Byte_num; i++) {
			middata <<= 8;
			middata += *pBuffer--;
		}
		return middata;
	}
}

void AT24C02_Read(u8 ReadAddr, u8 *pBuffer, u8 ReadNum)
{
	while (ReadNum--) {
		*pBuffer++ = AT24C02_ReadByte(ReadAddr++);
	}
}

void AT24C02_Write(u8 WriteAddr, u8 *pBuffer, u8 WriteNum)
{
	while (WriteNum--) {
		AT24C02_WriteByte(WriteAddr, *pBuffer);
		WriteAddr++;
		pBuffer++;
	}
}
