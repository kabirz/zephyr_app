#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(laser_spi, LOG_LEVEL_INF);

static const struct spi_dt_spec spi_spec =
	SPI_DT_SPEC_GET(DT_NODELABEL(spi_laser_fpga), SPI_WORD_SET(8), 0);

#ifdef CONFIG_SHELL
#include <zephyr/shell/shell.h>
static int cmd_version_get(const struct shell *ctx, size_t argc, char **argv)
{
	return 0;
}

static int cmd_spi_transceive(const struct shell *ctx, size_t argc, char **argv)
{
	uint8_t rx_buffer[8] = {0};
	uint8_t tx_buffer[8] = {0};

	int bytes_to_send = argc - 1;

	for (int i = 0; i < bytes_to_send; i++) {
		tx_buffer[i] = strtol(argv[1 + i], NULL, 16);
	}

	const struct spi_buf tx_buffers = {.buf = tx_buffer, .len = bytes_to_send};
	const struct spi_buf rx_buffers = {.buf = rx_buffer, .len = bytes_to_send};

	const struct spi_buf_set tx_buf_set = {.buffers = &tx_buffers, .count = 1};
	const struct spi_buf_set rx_buf_set = {.buffers = &rx_buffers, .count = 1};

	int ret = spi_transceive_dt(&spi_spec, &tx_buf_set, &rx_buf_set);

	if (ret < 0) {
		shell_error(ctx, "spi_transceive returned %d", ret);
		return ret;
	}

	shell_print(ctx, "TX:");
	shell_hexdump(ctx, tx_buffer, bytes_to_send);

	shell_print(ctx, "RX:");
	shell_hexdump(ctx, rx_buffer, bytes_to_send);

	return ret;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_biss_cmds,
			       SHELL_CMD_ARG(version, NULL,
					     "get biss version\n"
					     "Usage: version",
					     cmd_version_get, 1, 0),
			       SHELL_CMD_ARG(transfer, NULL,
					     "transfer bytes to an spi device\n"
					     "Usage: transfer <reg> [<byte1>, ...]",
					     cmd_spi_transceive, 1, 7),
			       SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(biss, &sub_biss_cmds, "Spi biss commands", NULL);
#endif
