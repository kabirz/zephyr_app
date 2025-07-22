#include <stdlib.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/sys/util.h>
#include <laser-common.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(laser_spi, LOG_LEVEL_INF);

enum {
	VERSION_GET_REG = 0,
	TIMESTAMP_REG,
	ENCODE1_GET_REG,
	ENCODE2_GET_REG,
	FPGA_RESET = 0xA5,
};

static const struct spi_dt_spec spi_spec =
	SPI_DT_SPEC_GET(DT_NODELABEL(spi_laser_fpga), SPI_WORD_SET(8), 0);

struct encode_msg {
	uint8_t reg;
  	uint32_t single_num:17;
  	uint32_t mutli_num:15;
  	uint32_t timecount;
} __PACKED;

struct encode_data {
	uint64_t timestamp;
	uint32_t timecount;
	uint32_t single_num;
	uint32_t mutli_num;
};
static struct encode_data encode_datas[4];

static int spi_tranfer(void *tx_buffer, void *rx_buffer, size_t size)
{
	const struct spi_buf tx_buffers = {.buf = tx_buffer, .len = size};
	const struct spi_buf rx_buffers = {.buf = rx_buffer, .len = size};

	const struct spi_buf_set tx_buf_set = {.buffers = &tx_buffers, .count = 1};
	const struct spi_buf_set rx_buf_set = {.buffers = &rx_buffers, .count = 1};

	return spi_transceive_dt(&spi_spec, &tx_buf_set, &rx_buf_set);
}

uint32_t fpga_uint32_get(uint8_t reg, uint32_t *val)
{
	uint8_t rx_buf[5] = {0}, tx_buf[5] = {0};

	tx_buf[0] = VERSION_GET_REG;
	if (spi_tranfer(tx_buf, rx_buf, sizeof(rx_buf))) {
		LOG_ERR("spi tranfer error");
		return -1;
	}
	*val = *(uint32_t *)(rx_buf+1);

	return 0;
}

uint32_t fpga_uint32_set(uint8_t reg, uint32_t val)
{
	uint8_t rx_buf[5] = {0}, tx_buf[5] = {0};

	tx_buf[0] = VERSION_GET_REG;
	*(uint32_t *)(tx_buf+1) = val;

	if (spi_tranfer(tx_buf, rx_buf, sizeof(rx_buf))) {
		LOG_ERR("spi tranfer error");
		return -1;
	}

	return 0;
}

static int encode_data_get(uint8_t reg, struct encode_msg *rx)
{
	struct encode_msg tx_buf;

	tx_buf.reg = reg;
	if (spi_tranfer(&tx_buf, rx, sizeof(tx_buf))) {
		LOG_ERR("spi tranfer error");
		return -1;
	}
	return 0;
}

int laser_get_encode_data(int32_t *encode1, int32_t *encode2)
{
	uint32_t fpga_time_diff, local_time_diff;

	if (encode_datas[1].timecount < encode_datas[0].timecount)
		fpga_time_diff = UINT32_MAX - encode_datas[0].timecount + encode_datas[1].timecount;
	else
		fpga_time_diff = encode_datas[1].timecount - encode_datas[0].timecount;

	local_time_diff = encode_datas[1].timestamp - encode_datas[0].timestamp;
	if ((fpga_time_diff -local_time_diff) > 800) {
		LOG_WRN("encode1: fpga diff: %d ms, local diff: %d ms", fpga_time_diff, local_time_diff);
		*encode1 = 0;
	} else {
		*encode1 = (int32_t)encode_datas[1].single_num * 360 + (int32_t)encode_datas[1].mutli_num;
	}

	if (encode_datas[3].timecount < encode_datas[2].timecount)
		fpga_time_diff = UINT32_MAX - encode_datas[2].timecount + encode_datas[3].timecount;
	else
		fpga_time_diff = encode_datas[3].timecount - encode_datas[2].timecount;
	local_time_diff = encode_datas[3].timestamp - encode_datas[2].timestamp;
	if ((fpga_time_diff -local_time_diff) > 800) {
		LOG_WRN("encode2: fpga diff: %d ms, local diff: %d ms", fpga_time_diff, local_time_diff);
		*encode2 = 0;
	} else {
		*encode2 = (int32_t)encode_datas[3].single_num * 360 + (int32_t)encode_datas[3].mutli_num;
	}
	return 0;
}

static void encode_process_thread(void)
{
	struct encode_msg msg[2];
	uint32_t version, timestamp;

	// FGPA reset
	fpga_uint32_set(FPGA_RESET, 0xA5);
	k_sleep(K_MSEC(20));

	// timestamp, version
	fpga_uint32_get(TIMESTAMP_REG, &timestamp);
	fpga_uint32_get(VERSION_GET_REG, &version);
	LOG_INF("FPAG Version: 0x%x, timestamp: %d ms", version, timestamp);
	while (true) {
		if (!atomic_test_bit(&laser_status, LASER_WRITE_MODE) &&
			!atomic_test_bit(&laser_status, LASER_FW_UPDATE)) {
			encode_data_get(ENCODE1_GET_REG, &msg[0]);
			encode_data_get(ENCODE2_GET_REG, &msg[1]);
			encode_datas[0] = encode_datas[1];
			encode_datas[1].timecount = msg[0].timecount;
			encode_datas[1].timestamp = k_uptime_get();
			if (msg[0].single_num & BIT(16)) // nagetive
				encode_datas[1].single_num = - (msg[0].single_num & BIT_MASK(16));
			else
				encode_datas[1].single_num = msg[0].single_num;
			if (msg[0].mutli_num & BIT(14)) // nagetive
				encode_datas[1].mutli_num = - (msg[0].mutli_num & BIT_MASK(14));
			else
				encode_datas[1].mutli_num = msg[0].mutli_num;

			encode_datas[2] = encode_datas[3];
			encode_datas[3].timecount = msg[1].timecount;
			encode_datas[3].timestamp = k_uptime_get();
			if (msg[0].single_num & BIT(16)) // nagetive
				encode_datas[3].single_num = - (msg[1].single_num & BIT_MASK(16));
			else
				encode_datas[3].single_num = msg[1].single_num;
			if (msg[0].mutli_num & BIT(14)) // nagetive
				encode_datas[3].mutli_num = - (msg[1].mutli_num & BIT_MASK(14));
			else
				encode_datas[3].mutli_num = msg[1].mutli_num;

			k_sleep(K_MSEC(10));
		} else {
			k_sleep(K_MSEC(300));
		}
	}
}
K_THREAD_DEFINE(encode_msg, 1024, encode_process_thread, NULL, NULL, NULL, 13, 0, 0);

#ifdef CONFIG_SHELL
#include <zephyr/shell/shell.h>
#include <stdlib.h>
static int shell_version_get(const struct shell *ctx, size_t argc, char **argv)
{
	uint32_t version;

	fpga_uint32_get(VERSION_GET_REG, &version);

	shell_print(ctx, "fpge version: 0x%x", version);
	return 0;
}

static int shell_encode_get(const struct shell *ctx, size_t argc, char **argv)
{
	struct encode_msg rx_msg;
	uint8_t reg;

	switch (strtol(argv[1], NULL, 0)) {
	case 1:
		reg = ENCODE1_GET_REG;
		break;
	case 2:
		reg = ENCODE2_GET_REG;
		break;
	default:
		shell_print(ctx, "Usage: %s <1/2>", argv[0]);
		return -1;
	}
	if (encode_data_get(reg, &rx_msg) == 0) {
		int32_t single_num, mutli_num;
		if (rx_msg.single_num & BIT(16)) // nagetive
			single_num = - (rx_msg.single_num & BIT_MASK(16));
		else
			single_num = rx_msg.single_num;
		if (rx_msg.mutli_num & BIT(14)) // nagetive
			mutli_num = - (rx_msg.mutli_num & BIT_MASK(14));
		else
			mutli_num = rx_msg.mutli_num;
		shell_print(ctx, "timestamp: %d ms, single num: %d, mutil num: %d, (0x%x, 0x%x, 0x%x)",
			rx_msg.timecount, single_num, mutli_num,
			rx_msg.timecount, rx_msg.single_num, rx_msg.mutli_num);
		shell_print(ctx, "TX:");
	      	shell_hexdump(ctx, (void *)&rx_msg, sizeof(rx_msg));
	}
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_fpga_cmds,
			       SHELL_CMD_ARG(version, NULL,
					     "get fpga version\n"
					     "Usage: version",
					     shell_version_get, 1, 0),
			       SHELL_CMD_ARG(encode, NULL,
					     "get encode data\n"
					     "Usage: encode <1/2>",
					     shell_encode_get, 2, 0),
			       SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(fpage_encode, &sub_fpga_cmds, "fpga encode commands", NULL);
#endif

