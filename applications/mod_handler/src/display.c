#include <common.h>
#include <zephyr/sys/util.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(mod_display, LOG_LEVEL_INF);

static const struct device *display_dev = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
static struct display_capabilities capabilities;
static struct display_buffer_descriptor buf_desc;

int mod_display_init(void)
{
	if (!device_is_ready(display_dev)) {
		LOG_ERR("Device %s not found.", display_dev->name);
		return -1;
	}

	display_get_capabilities(display_dev, &capabilities);
	LOG_INF("Display %dx%d\n", capabilities.x_resolution, capabilities.y_resolution);

	display_clear(display_dev);
	display_blanking_off(display_dev);

	return 0;
}
static const uint8_t bitmap_bytes[][32] = {
/* 来自： https://www.23bei.com/tool/965.html
 * [字库]：[HZK1616宋体]
 * [数据排列]:从左到右从上到下
 * [取模方式]:纵向8点上高位
 * [正负反色]:否
 * [去掉重复后]共2个字符
 * [总字符库]："你好"
 */

	/*-- ID:0,字符:"你",ASCII编码:C4E3,对应字:宽x高=16x16,画布:宽W=16 高H=16,共32字节*/
	{
		0x02,0x04,0x1F,0xE0,0x02,0x04,0x18,0xF0,0x10,0x13,0x10,0x10,0x14,0x18,0x00,0x00,
		0x00,0x00,0xFF,0x00,0x00,0x10,0x20,0xC2,0x01,0xFE,0x00,0x80,0x60,0x30,0x00,0x00
	},

	/*-- ID:1,字符:"好",ASCII编码:BAC3,对应字:宽x高=16x16,画布:宽W=16 高H=16,共32字节*/
	{
		0x08,0x08,0x0F,0xF8,0x08,0x0F,0x01,0x41,0x41,0x41,0x47,0x49,0x51,0x63,0x01,0x00,
		0x02,0x44,0xA8,0x10,0x28,0xC6,0x00,0x00,0x02,0x01,0xFE,0x00,0x00,0x00,0x00,0x00
	},
};

void mod_display_16x16(const uint8_t *data, int x, int y)
{
	buf_desc.buf_size = 32;
	buf_desc.height = 16;
	buf_desc.width = 16;
	buf_desc.pitch = 16;
	display_write(display_dev, x, y, &buf_desc, data);
}

void mod_display_demo(void)
{
	for (int h = 0; h < capabilities.y_resolution; h += 16) {
		for (int w = 0; w < capabilities.x_resolution; w += 16) {
			mod_display_16x16(bitmap_bytes[(w % 32) == 16], w, h);
		}
	}
}

void mod_display_battery(const gloval_params_t *params)
{
}

void mod_display_handler_x(const gloval_params_t *params)
{
}

void mod_display_handler_y(const gloval_params_t *params)
{
}

void mod_display_handler_button(const gloval_params_t *params)
{
}

void mod_display_lora_can(const gloval_params_t *params)
{
}

void mod_display_all(const gloval_params_t *params)
{
}
