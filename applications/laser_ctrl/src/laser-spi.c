#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(laser_spi, LOG_LEVEL_INF);

#define VERSION_GET_REG 0x00
#define XAXIS_GET_REG   0x04
#define YAXIS_GET_REG   0x08

static const struct spi_dt_spec spi_spec =
	SPI_DT_SPEC_GET(DT_NODELABEL(spi_laser_fpga), SPI_WORD_SET(8), 0);

static int spi_tranfer(uint8_t *tx_buffer, uint8_t *rx_buffer, size_t size)
{
	const struct spi_buf tx_buffers = {.buf = tx_buffer, .len = size};
	const struct spi_buf rx_buffers = {.buf = rx_buffer, .len = size};

	const struct spi_buf_set tx_buf_set = {.buffers = &tx_buffers, .count = 1};
	const struct spi_buf_set rx_buf_set = {.buffers = &rx_buffers, .count = 1};

	return spi_transceive_dt(&spi_spec, &tx_buf_set, &rx_buf_set);
}

int fpga_version_get(void)
{
	uint8_t rx_buf[5] = {0}, tx_buf[5] = {0};

	tx_buf[0] = VERSION_GET_REG;
	if (spi_tranfer(tx_buf, rx_buf, sizeof(rx_buf))) {
		LOG_ERR("spi tranfer error");
		return -1;
	} else {
		LOG_HEXDUMP_INF(rx_buf, sizeof(rx_buf), "version");
	}
	return 0;
}

int Xaxis_data_get(void)
{
	uint8_t rx_buf[9] = {0}, tx_buf[9] = {0};

	tx_buf[0] = XAXIS_GET_REG;
	if (spi_tranfer(tx_buf, rx_buf, sizeof(rx_buf))) {
		LOG_ERR("spi tranfer error");
		return -1;
	} else {
		LOG_HEXDUMP_INF(rx_buf, sizeof(rx_buf), "Xaxis");
	}
	return 0;
}

int Yaxis_data_get(void)
{
	uint8_t rx_buf[9] = {0}, tx_buf[9] = {0};

	tx_buf[0] = YAXIS_GET_REG;
	if (spi_tranfer(tx_buf, rx_buf, sizeof(rx_buf))) {
		LOG_ERR("spi tranfer error");
		return -1;
	} else {
		LOG_HEXDUMP_INF(rx_buf, sizeof(rx_buf), "Yaxis");
	}
	return 0;
}

#ifdef CONFIG_SHELL
#include <zephyr/shell/shell.h>
static int shell_version_get(const struct shell *ctx, size_t argc, char **argv)
{
	fpga_version_get();
	return 0;
}
static int shell_Xaxis_get(const struct shell *ctx, size_t argc, char **argv)
{
	Xaxis_data_get();
	return 0;
}

static int shell_Yaxis_get(const struct shell *ctx, size_t argc, char **argv)
{
	Yaxis_data_get();
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_biss_cmds,
			       SHELL_CMD_ARG(version, NULL,
					     "get biss version\n"
					     "Usage: version",
					     shell_version_get, 2, 0),
			       SHELL_CMD_ARG(xaxis, NULL,
					     "get x axis data\n"
					     "Usage: xaxis",
					     shell_Xaxis_get, 2, 0),
			       SHELL_CMD_ARG(yaxis, NULL,
					     "get y axis data\n"
					     "Usage: xaxis",
					     shell_Yaxis_get, 2, 0),
			       SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(biss, &sub_biss_cmds, "Spi biss commands", NULL);
#endif

