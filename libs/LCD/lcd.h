#ifndef __LCD_H
#define __LCD_H
#include <stdint.h>
#include <zephyr/kernel.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef volatile uint16_t vu16;
typedef uint32_t u32;

extern u16 lcd_id;
extern u8 dir_flag;
extern u16 lcd_width;
extern u16 lcd_height;
extern u16 write_gramcmd;
extern u16 setxcmd;
extern u16 setycmd;

extern u16 BRUSH_COLOR;
extern u16 BACK_COLOR;

#define DEV_BACK_GPIO DEVICE_DT_GET(DT_NODELABEL(gpiof))
#define LCD_BACK(v) gpio_pin_set(DEV_BACK_GPIO, 10, v)

#define CMD_BASE  ((u32)(0x6C000000 | 0x00001FFE))
#define DATA_BASE ((u32)(0x6C000000 | 0x00002000))

#define LCD_CMD  (*(vu16 *)CMD_BASE)
#define LCD_DATA (*(vu16 *)DATA_BASE)

#define L2R_U2D 0
#define L2R_D2U 1
#define R2L_U2D 2
#define R2L_D2U 3

#define U2D_L2R 4
#define U2D_R2L 5
#define D2U_L2R 6
#define D2U_R2L 7

#define INIT_SCAN_DIR L2R_U2D

#define WHITE   0xFFFF
#define BLACK   0x0000
#define BLUE    0x001F
#define GREEN   0x07E0
#define BRED    0XF81F
#define GRED    0XFFE0
#define GBLUE   0X07FF
#define BROWN   0XBC40
#define BRRED   0XFC07
#define GRAY    0X8430
#define RED     0xF800
#define MAGENTA 0xF81F
#define CYAN    0x7FFF
#define YELLOW  0xFFE0
#define ILI9341_W 0x240
#define ILI9341_H 0x320
#define ILI9341_GAMSET 0x26
#define ILI9341_IFMODE 0xB0
#define ILI9341_FRMCTR1 0xB1
#define ILI9341_DISCTRL 0xB6
#define ILI9341_ETMOD 0xB7
#define ILI9341_PWCTRL1 0xC0
#define ILI9341_PWCTRL2 0xC1
#define ILI9341_VMCTRL1 0xC5
#define ILI9341_VMCTRL2 0xC7
#define ILI9341_PWCTRLA 0xCB
#define ILI9341_PWCTRLB 0xCF
#define ILI9341_PGAMCTRL 0xE0
#define ILI9341_NGAMCTRL 0xE1
#define ILI9341_TIMCTRLA 0xE8
#define ILI9341_TIMCTRLB 0xEA
#define ILI9341_PWSEQCTRL 0xED
#define ILI9341_ENABLE3G 0xF2
#define ILI9341_IFCTL 0xF6
#define ILI9341_PUMPRATIOCTRL 0xF7
#define ILI9XXX_SWRESET 0x01
#define ILI9XXX_SLPOUT 0x11
#define ILI9XXX_DINVON 0x21
#define ILI9XXX_GAMSET 0x26
#define ILI9XXX_DISPOFF 0x28
#define ILI9XXX_DISPON 0x29
#define ILI9XXX_CASET 0x2a
#define ILI9XXX_PASET 0x2b
#define ILI9XXX_RAMWR 0x2c
#define ILI9XXX_RGBSET 0x2d
#define ILI9XXX_RAMRD 0x2e
#define ILI9XXX_MADCTL 0x36
#define ILI9XXX_PIXSET 0x3A
#define ILI9XXX_RAMRD_CONT 0x3e

void LCD_WriteReg(u16 LCD_Reg, u16 LCD_Value);
u16 LCD_ReadReg(u16 LCD_Reg);

void LCD_Init(void);
void LCD_DisplayOn(void);
void LCD_DisplayOff(void);
void LCD_Clear(u16 Color);
void LCD_SetCursor(u16 Xpos, u16 Ypos);
void LCD_DrawPoint(u16 x, u16 y);
void LCD_Color_DrawPoint(u16 x, u16 y, u16 color);
u16 LCD_GetPoint(u16 x, u16 y);

void LCD_AUTOScan_Dir(u8 dir);
void LCD_Display_Dir(u8 dir);
void LCD_Set_Window(u16 sx, u16 sy, u16 width, u16 height);

void LCD_Fill_onecolor(u16 sx, u16 sy, u16 ex, u16 ey, u16 color);
void LCD_DisplayChar(u16 x, u16 y, u8 word, u8 size);
void LCD_DisplayNum(u16 x, u16 y, u32 num, u8 len, u8 size, u8 mode);
void LCD_DisplayNum_color(u16 x, u16 y, u32 num, u8 len, u8 size, u8 mode, u16 brushcolor, u16 backcolor);
void LCD_DisplayString(u16 x, u16 y, u8 size, u8 *p);
void LCD_DisplayString_color(u16 x, u16 y, u8 size, u8 *p, u16 brushcolor, u16 backcolor);

#endif
