#include <zephyr/shell/shell.h>
#include "LCD/lcd.h"
#include "24C02/24c02.h"

static int i2c_init(const struct shell *ctx, size_t argc, char **argv)
{
	AT24C02_Init();
	return 0;
}
static int i2c_read(const struct shell *ctx, size_t argc, char **argv)
{
	u8 data[16];
	AT24C02_Read(0, data, 16);
	shell_hexdump(ctx, data, 16);
	return 0;
}

static int i2c_write(const struct shell *ctx, size_t argc, char **argv)
{
	AT24C02_Write(0, argv[1], strlen(argv[1]));
	shell_print(ctx, "write: %s", argv[1]);
	shell_hexdump(ctx, argv[1], strlen(argv[1]));

	return 0;
}

static int lcd_init(const struct shell *ctx, size_t argc, char **argv)
{
	LCD_Init();
	return 0;
}

static int lcd_write_str(const struct shell *ctx, size_t argc, char **argv)
{
	BRUSH_COLOR = RED;
	LCD_DisplayString(0, 0, strlen(argv[1]), argv[1]);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(i2c_cmds,
			       SHELL_CMD_ARG(init, NULL,
					     "init\n"
					     "Usage: init",
					     i2c_init, 1, 0),
			       SHELL_CMD_ARG(read, NULL,
					     "i2c read\n"
					     "Usage: read",
					     i2c_read, 1, 0),
			       SHELL_CMD_ARG(write, NULL,
					     "i2c write\n"
					     "Usage: write str",
					     i2c_write, 2, 0),
			       SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(lib_i2c, &i2c_cmds, "i2c commands", NULL);

SHELL_STATIC_SUBCMD_SET_CREATE(lcd_cmds,
			       SHELL_CMD_ARG(init, NULL,
					     "init\n"
					     "Usage: init",
					     lcd_init, 1, 0),
			       SHELL_CMD_ARG(write, NULL,
					     "write\n"
					     "Usage: write str",
					     lcd_write_str, 2, 0),
			       SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(lib_lcd, &lcd_cmds, "lcd commands", NULL);
