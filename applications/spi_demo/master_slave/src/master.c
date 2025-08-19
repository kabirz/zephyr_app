#include <zephyr/kernel.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/shell/shell.h>

static const struct device *spi_dev = DEVICE_DT_GET(DT_NODELABEL(spi1));

static uint8_t tx_buffer[128];
static uint8_t rx_buffer[128];

static int cmd_mspi_write(const struct shell *ctx, size_t argc, char **argv)
{
	const struct spi_config config = {
		.frequency = MHZ(1),
		.cs = {
			.gpio = GPIO_DT_SPEC_GET(DT_NODELABEL(spi1), cs_gpios),
		},
	};

	const struct spi_buf tx_buffers = {.buf = tx_buffer, .len = sizeof(tx_buffer)};
	const struct spi_buf rx_buffers = {.buf = rx_buffer, .len = sizeof(rx_buffer)};

	const struct spi_buf_set tx_buf_set = {.buffers = &tx_buffers, .count = 1};
	const struct spi_buf_set rx_buf_set = {.buffers = &rx_buffers, .count = 1};

	memcpy(tx_buffer, argv[1], strlen(argv[1])+1);

	if (spi_transceive(spi_dev, &config, &tx_buf_set, &rx_buf_set)) {
		shell_error(ctx, "spi transfer error");
		return -1;
	} else {
		shell_hexdump(ctx, rx_buffer, sizeof(rx_buffer));
	}

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_mspi_cmds,
			       SHELL_CMD_ARG(write, NULL,
					     "master spi write\n"
					     "Usage: write <str>",
					     cmd_mspi_write, 2, 0),
			       SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(mspi, &sub_mspi_cmds, "Spi Flash commands", NULL);
