#include "lcd.h"
#include "cfont.h"
#include "zephyr/arch/common/sys_io.h"
#include <zephyr/sys/sys_io.h>
#include <zephyr/kernel.h>
u16 BRUSH_COLOR = BLACK;
u16 BACK_COLOR = WHITE;

u8 dir_flag;
u16 lcd_width;
u16 lcd_height;

struct ili9341_regs {
	uint8_t reg;
	uint8_t len;
	uint8_t vals[16];
} regs[] = {
	{ILI9341_PWCTRLB, 3, {0x00, 0xc1, 0x30}},
	{ILI9341_PWSEQCTRL, 4, {0x64, 0x03, 0x12, 0x81}},
	{ILI9341_TIMCTRLA, 3, {0x85, 0x10, 0x7a}},
	{ILI9341_PWCTRLA, 5, {0x39, 0x2C, 0x00, 0x34, 0x02}},
	{ILI9341_PUMPRATIOCTRL, 1, {0x20}},
	{ILI9341_TIMCTRLB, 2, {0x00, 0x00}},
	{ILI9341_PWCTRL1, 1, {0x1b}},
	{ILI9341_PWCTRL2, 1, {0x01}},
	{ILI9341_VMCTRL1, 2, {0x30, 0x30}},
	{ILI9341_VMCTRL2, 1, {0xB7}},
	{ILI9XXX_MADCTL, 1, {0x48}},
	{ILI9XXX_PIXSET, 1, {0x55}},
	{ILI9341_FRMCTR1, 2, {0x00, 0x1a}},
	{ILI9341_DISCTRL, 2, {0x0a, 0xa2}},
	{ILI9341_ENABLE3G, 1, {0x00}},
	{ILI9341_GAMSET, 1, {0x01}},
	{ILI9341_PGAMCTRL, 15, {0x0f, 0x2a, 0x28, 0x08, 0x0e, 0x08, 0x54, 0xa9, 0x43, 0x0a, 0x0f, 0x00, 0x00, 0x00, 0x00}},
	{ILI9341_NGAMCTRL, 15, {0x00, 0x15, 0x17, 0x07, 0x11, 0x06, 0x2B, 0x56, 0x3C, 0x05, 0x10, 0x0F, 0x3F, 0x3F, 0x0F}},
	{ILI9XXX_CASET, 4, {0x00, 0x00, 0x00, 0xef}},
	{ILI9XXX_PASET, 4, {0x00, 0x00, 0x01, 0x3f}},
};

void LCD_WriteReg(u16 LCD_Reg, u16 LCD_Value)
{
	sys_write16(LCD_Reg, LCD_CMD);
	sys_write16(LCD_Value, LCD_DATA);
}

void LCD_WriteRegs(struct ili9341_regs *reg)
{
	sys_write16(reg->reg, LCD_CMD);
	for (int i = 0; i < reg->len; i++) {
		sys_write16(reg->vals[i], LCD_DATA);
	}
}

u16 LCD_ReadReg(u16 LCD_Reg)
{
	sys_write16(LCD_Reg, LCD_CMD);
	k_busy_wait(5);
	return sys_read16(LCD_DATA);
}

void lcdm_delay(u8 i)
{
	while (i--) ;
}

u16 LCD_BGRtoRGB(u16 bgr)
{
	u16 r, g, b, rgb;
	b = (bgr >> 0) & 0x1f;
	g = (bgr >> 5) & 0x3f;
	r = (bgr >> 11) & 0x1f;
	rgb = (b << 11) + (g << 5) + (r << 0);
	return (rgb);
}

u16 LCD_GetPoint(u16 x, u16 y)
{
	volatile u16 r = 0, g = 0, b = 0;

	if (x >= lcd_width || y >= lcd_height) {
		return 0;
	}
	LCD_SetCursor(x, y);
	sys_write16(ILI9XXX_RAMRD, LCD_CMD);
	if (sys_read16(LCD_DATA)) {
		r = 0;
	}
	lcdm_delay(2);
	r = sys_read16(LCD_DATA);
	lcdm_delay(2);
	b = sys_read16(LCD_DATA);
	g = r & 0XFF;
	g <<= 8;
	return (((r >> 11) << 11) | ((g >> 10) << 5) | (b >> 11));
}

void LCD_DisplayOn(void)
{
	sys_write16(ILI9XXX_DISPON, LCD_CMD);
}

void LCD_DisplayOff(void)
{
	sys_write16(ILI9XXX_DISPOFF, LCD_CMD);
}

void LCD_SetCursor(u16 Xaddr, u16 Yaddr)
{
	sys_write16(ILI9XXX_CASET, LCD_CMD);
	sys_write16(Xaddr >> 8, LCD_DATA);
	sys_write16(Xaddr & 0xFF, LCD_DATA);
	sys_write16(ILI9XXX_PASET, LCD_CMD);
	sys_write16(Yaddr >> 8, LCD_DATA);
	sys_write16(Yaddr & 0xFF, LCD_DATA);
}

void LCD_AUTOScan_Dir(u8 dir)
{
	u16 regval = 0;
	u16 dirreg = 0;
	u16 temp;

	if (dir_flag == 1) {
		switch (dir) {
		case 0:
			dir = 6;
			break;
		case 1:
			dir = 7;
			break;
		case 2:
			dir = 4;
			break;
		case 3:
			dir = 5;
			break;
		case 4:
			dir = 1;
			break;
		case 5:
			dir = 0;
			break;
		case 6:
			dir = 3;
			break;
		case 7:
			dir = 2;
			break;
		}
	}
	switch (dir) {
	case L2R_U2D:
		regval |= (0 << 7) | (0 << 6) | (0 << 5);
		break;
	case L2R_D2U:
		regval |= (1 << 7) | (0 << 6) | (0 << 5);
		break;
	case R2L_U2D:
		regval |= (0 << 7) | (1 << 6) | (0 << 5);
		break;
	case R2L_D2U:
		regval |= (1 << 7) | (1 << 6) | (0 << 5);
		break;
	case U2D_L2R:
		regval |= (0 << 7) | (0 << 6) | (1 << 5);
		break;
	case U2D_R2L:
		regval |= (0 << 7) | (1 << 6) | (1 << 5);
		break;
	case D2U_L2R:
		regval |= (1 << 7) | (0 << 6) | (1 << 5);
		break;
	case D2U_R2L:
		regval |= (1 << 7) | (1 << 6) | (1 << 5);
		break;
	}
	dirreg = 0X36;
	regval |= 0X08;
	LCD_WriteReg(dirreg, regval);

	if (regval & 0X20) {
		if (lcd_width < lcd_height)
		{
			temp = lcd_width;
			lcd_width = lcd_height;
			lcd_height = temp;
		}
	}
	sys_write16(ILI9XXX_CASET, LCD_CMD);
	sys_write16(0, LCD_DATA);
	sys_write16(0, LCD_DATA);
	sys_write16((lcd_width - 1) >> 8, LCD_DATA);
	sys_write16((lcd_width - 1) & 0XFF, LCD_DATA);

	sys_write16(ILI9XXX_PASET, LCD_CMD);
	sys_write16(0, LCD_DATA);
	sys_write16(0, LCD_DATA);
	sys_write16((lcd_height - 1) >> 8, LCD_DATA);
	sys_write16((lcd_height - 1) & 0XFF, LCD_DATA);
}

void LCD_Display_Dir(u8 dir)
{
	dir_flag = !!dir;
	lcd_width = ILI9341_W;
	lcd_height = ILI9341_H;
	LCD_AUTOScan_Dir(INIT_SCAN_DIR);
}

void LCD_DrawPoint(u16 x, u16 y)
{
	LCD_SetCursor(x, y);
	sys_write16(ILI9XXX_RAMWR, LCD_CMD);
	sys_write16(BRUSH_COLOR, LCD_DATA);
}

void LCD_Color_DrawPoint(u16 x, u16 y, u16 color)
{
	sys_write16(ILI9XXX_CASET, LCD_CMD);
	sys_write16(x>>8, LCD_DATA);
	sys_write16(x&0xFF, LCD_DATA);

	sys_write16(ILI9XXX_PASET, LCD_CMD);
	sys_write16(y>>8, LCD_DATA);
	sys_write16(y&0xFF, LCD_DATA);

	sys_write16(ILI9XXX_RAMWR, LCD_CMD);
	sys_write16(color, LCD_DATA);
}

void LCD_Set_Window(u16 sx, u16 sy, u16 width, u16 height)
{
	width = sx + width - 1;
	height = sy + height - 1;

	sys_write16(ILI9XXX_CASET, LCD_CMD);
	sys_write16(sx>>8, LCD_DATA);
	sys_write16(sx&0xff, LCD_DATA);
	sys_write16(width >> 8, LCD_DATA);
	sys_write16(width & 0XFF, LCD_DATA);

	sys_write16(ILI9XXX_PASET, LCD_CMD);
	sys_write16(sy>>8, LCD_DATA);
	sys_write16(sy&0xff, LCD_DATA);
	sys_write16(height >> 8, LCD_DATA);
	sys_write16(height & 0XFF, LCD_DATA);
}

void LCD_Init(void)
{
	LCD_WriteReg(0x0000, 0x0001);
	k_busy_wait(5);

	for (int i = 0; i < ARRAY_SIZE(regs); i++) {
		LCD_WriteRegs(&regs[i]);
	}

	sys_write16(ILI9XXX_SLPOUT, LCD_CMD);
	k_msleep(50);
	sys_write16(ILI9XXX_DISPON, LCD_CMD);
	LCD_Display_Dir(0);
	LCD_Clear(WHITE);
}

void LCD_Clear(u16 color)
{
	u32 i = 0;
	u32 pointnum = 0;

	pointnum = lcd_width * lcd_height;
	LCD_SetCursor(0x00, 0x00);
	sys_write16(ILI9XXX_RAMWR, LCD_CMD);
	for (i = 0; i < pointnum; i++) {
		sys_write16(color, LCD_DATA);
	}
}

void LCD_Fill_onecolor(u16 sx, u16 sy, u16 ex, u16 ey, u16 color)
{
	u16 i, j;
	u16 nlen = 0;

	nlen = ex - sx + 1;
	for (i = sy; i <= ey; i++) {
		LCD_SetCursor(sx, i);
		sys_write16(ILI9XXX_RAMWR, LCD_CMD);
		for (j = 0; j < nlen; j++) {
			sys_write16(color, LCD_DATA);
		}
	}
}

void LCD_DisplayChar(u16 x, u16 y, u8 word, u8 size)
{
	u8 bytenum, bytedata, a, b;
	u16 ymid = y;

	if (size == 12) {
		bytenum = 12;
	} else if (size == 16) {
		bytenum = 16;
	} else if (size == 24) {
		bytenum = 36;
	} else {
		return;
	}

	word = word - ' ';

	for (b = 0; b < bytenum; b++) {
		if (size == 12) {
			bytedata = char_1206[word][b];
		} else if (size == 16) {
			bytedata = char_1608[word][b];
		} else if (size == 24) {
			bytedata = char_2412[word][b];
		} else {
			return;
		}
		for (a = 0; a < 8; a++) {
			if (bytedata & 0x80) {
				LCD_Color_DrawPoint(x, y, BRUSH_COLOR);
			} else {
				LCD_Color_DrawPoint(x, y, BACK_COLOR);
			}
			bytedata <<= 1;
			y++;
			if (y >= lcd_height) {
				return;
			}
			if ((y - ymid) == size) {
				y = ymid;
				x++;
				if (x >= lcd_width) {
					return;
				}
				break;
			}
		}
	}
}

u32 LCD_Pow(u8 m, u8 n)
{
	u32 mid = 1;
	while (n--) {
		mid *= m;
	}
	return mid;
}

void LCD_DisplayNum(u16 x, u16 y, u32 num, u8 len, u8 size, u8 mode)
{
	u8 t, numtemp;
	u8 end0 = 0;
	for (t = 0; t < len; t++) {
		numtemp = (num / LCD_Pow(10, len - t - 1)) % 10;
		if (end0 == 0 && t < (len - 1)) {
			if (numtemp == 0) {
				if (mode) {
					LCD_DisplayChar(x + (size / 2) * t, y, '0', size);
				} else {
					LCD_DisplayChar(x + (size / 2) * t, y, ' ', size);
				}
				continue;
			} else {
				end0 = 1;
			}
		}
		LCD_DisplayChar(x + (size / 2) * t, y, numtemp + '0', size);
	}
}

void LCD_DisplayNum_color(u16 x, u16 y, u32 num, u8 len, u8 size, u8 mode, u16 brushcolor,
			  u16 backcolor)
{
	u16 bh_color, bk_color;

	bh_color = BRUSH_COLOR;
	bk_color = BACK_COLOR;

	BRUSH_COLOR = brushcolor;
	BACK_COLOR = backcolor;

	LCD_DisplayNum(x, y, num, len, size, mode);

	BRUSH_COLOR = bh_color;
	BACK_COLOR = bk_color;
}

void LCD_DisplayString(u16 x, u16 y, u8 size, u8 *p)
{
	while ((*p <= '~') && (*p >= ' '))
	{
		LCD_DisplayChar(x, y, *p, size);
		x += size / 2;
		if (x >= lcd_width) {
			break;
		}
		p++;
	}
}

void LCD_DisplayString_color(u16 x, u16 y, u8 size, u8 *p, u16 brushcolor, u16 backcolor)
{
	u16 bh_color, bk_color;

	bh_color = BRUSH_COLOR;
	bk_color = BACK_COLOR;

	BRUSH_COLOR = brushcolor;
	BACK_COLOR = backcolor;

	LCD_DisplayString(x, y, size, p);

	BRUSH_COLOR = bh_color;
	BACK_COLOR = bk_color;
}
