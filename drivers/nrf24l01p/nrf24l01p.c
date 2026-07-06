/* nRF24L01+ Zephyr 驱动实现 */
#define DT_DRV_COMPAT nordic_nrf24l01p

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <errno.h>
#include <string.h>

#include "nrf24l01p.h"
#include "nrf24l01p_reg.h"

LOG_MODULE_REGISTER(nrf24l01p, CONFIG_NRF24L01P_LOG_LEVEL);

/* mode 0, MSB, 8-bit */
#define NRF24_SPI_OPERATION  (SPI_WORD_SET(8) | SPI_TRANSFER_MSB)

struct nrf24_config {
	struct spi_dt_spec bus;
	struct gpio_dt_spec ce;
	struct gpio_dt_spec irq;

	uint8_t channel;
	const char *data_rate;      /* DT 字符串，运行时 parse */
	const char *tx_power;
	uint8_t address_width;
	const char *payload_mode;
	uint8_t rx_payload_width;
	const char *crc_mode;
	uint8_t ard;            /* ARD 原始 4-bit 编码 */
	uint8_t arc;
	uint8_t tx_addr[5];
};

struct nrf24_data {
	struct k_sem irq_sem;
	struct k_mutex lock;
	struct gpio_callback irq_cb;
	enum nrf24_mode mode;
	bool ready;
};

/* —— DT 字符串枚举解析 —— */
static enum nrf24_data_rate parse_data_rate(const char *s)
{
	if (!strcmp(s, "250k")) {
		return NRF24_RATE_250K;
	}
	if (!strcmp(s, "2m")) {
		return NRF24_RATE_2M;
	}
	return NRF24_RATE_1M;
}

static enum nrf24_tx_power parse_tx_power(const char *s)
{
	if (!strcmp(s, "-18dbm")) {
		return NRF24_PWR_18DBM;
	}
	if (!strcmp(s, "-12dbm")) {
		return NRF24_PWR_12DBM;
	}
	if (!strcmp(s, "-6dbm")) {
		return NRF24_PWR_6DBM;
	}
	return NRF24_PWR_0DBM;
}

static enum nrf24_payload_mode parse_payload_mode(const char *s)
{
	return (!strcmp(s, "fixed")) ? NRF24_PAYLOAD_FIXED : NRF24_PAYLOAD_DYNAMIC;
}

static enum nrf24_crc_mode parse_crc_mode(const char *s)
{
	return (!strcmp(s, "1-byte")) ? NRF24_CRC_1_BYTE : NRF24_CRC_2_BYTE;
}

/* —— 内部辅助（调用方须已持 data->lock）—— */
static inline const struct nrf24_config *get_cfg(const struct device *dev)
{
	return dev->config;
}

static inline struct nrf24_data *get_data(const struct device *dev)
{
	return dev->data;
}

/* 单次 SPI 收发（CSN 由控制器自动管理，命令+数据在同一 CSN 窗口）*/
static int nrf24_xfer(const struct device *dev, const uint8_t *tx, uint8_t *rx, size_t len)
{
	const struct spi_dt_spec *bus = &get_cfg(dev)->bus;
	struct spi_buf tx_buf = { .buf = (void *)tx, .len = len };
	struct spi_buf rx_buf = { .buf = rx, .len = len };
	const struct spi_buf_set txs = { .buffers = &tx_buf, .count = 1 };
	const struct spi_buf_set rxs = { .buffers = &rx_buf, .count = 1 };
	int ret = spi_transceive_dt(bus, &txs, &rxs);

	if (ret < 0) {
		LOG_ERR("SPI transceive failed: %d", ret);
	}
	return ret;
}

static int reg_read_locked(const struct device *dev, uint8_t reg, uint8_t *val)
{
	uint8_t tx[2] = { R_REGISTER | (reg & 0x1F), NOP };
	uint8_t rx[2] = { 0 };
	int ret = nrf24_xfer(dev, tx, rx, sizeof(tx));

	if (ret == 0) {
		*val = rx[1];
	}
	return ret;
}

static int reg_write_locked(const struct device *dev, uint8_t reg, uint8_t val)
{
	uint8_t tx[2] = { W_REGISTER | (reg & 0x1F), val };

	return nrf24_xfer(dev, tx, NULL, sizeof(tx));
}

static int reg_read_multi_locked(const struct device *dev, uint8_t reg, uint8_t *buf, size_t len)
{
	uint8_t nop[NRF24_MAX_PAYLOAD + 1] = { 0 };
	uint8_t rxb[NRF24_MAX_PAYLOAD + 1] = { 0 };

	if (len == 0 || len > NRF24_MAX_PAYLOAD) {
		return -EINVAL;
	}
	nop[0] = R_REGISTER | (reg & 0x1F);
	int ret = nrf24_xfer(dev, nop, rxb, len + 1);

	if (ret == 0) {
		memcpy(buf, &rxb[1], len);
	}
	return ret;
}

static int reg_write_multi_locked(const struct device *dev, uint8_t reg, const uint8_t *buf, size_t len)
{
	uint8_t txb[NRF24_MAX_PAYLOAD + 1];

	if (len == 0 || len > NRF24_MAX_PAYLOAD) {
		return -EINVAL;
	}
	txb[0] = W_REGISTER | (reg & 0x1F);
	memcpy(&txb[1], buf, len);
	return nrf24_xfer(dev, txb, NULL, len + 1);
}

static int cmd_locked(const struct device *dev, uint8_t cmd)
{
	return nrf24_xfer(dev, &cmd, NULL, 1);
}

static int read_rx_pl_wid_locked(const struct device *dev, uint8_t *wid)
{
	uint8_t tx[2] = { R_RX_PL_WID, NOP };
	uint8_t rx[2] = { 0 };
	int ret = nrf24_xfer(dev, tx, rx, sizeof(tx));

	if (ret == 0) {
		*wid = rx[1];
	}
	return ret;
}

static int read_rx_payload_locked(const struct device *dev, uint8_t *buf, uint8_t len)
{
	uint8_t nop[NRF24_MAX_PAYLOAD + 1];
	uint8_t rxb[NRF24_MAX_PAYLOAD + 1];

	memset(nop, NOP, sizeof(nop));
	nop[0] = R_RX_PAYLOAD;
	int ret = nrf24_xfer(dev, nop, rxb, len + 1);

	if (ret == 0) {
		memcpy(buf, &rxb[1], len);
	}
	return ret;
}

static int write_tx_payload_locked(const struct device *dev, const uint8_t *buf, uint8_t len)
{
	uint8_t txb[NRF24_MAX_PAYLOAD + 1];

	txb[0] = W_TX_PAYLOAD;
	memcpy(&txb[1], buf, len);
	return nrf24_xfer(dev, txb, NULL, len + 1);
}

static inline void ce_set(const struct device *dev, int val)
{
	gpio_pin_set_dt(&get_cfg(dev)->ce, val);
}

static void nrf24_irq_handler(const struct device *port, struct gpio_callback *cb,
			      uint32_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(pins);

	struct nrf24_data *d = CONTAINER_OF(cb, struct nrf24_data, irq_cb);

	/* ISR 内不做 SPI 事务，仅唤醒等待者 */
	k_sem_give(&d->irq_sem);
}

/* 检查 SPI/GPIO 就绪 + 配置 CE 输出、IRQ 输入与下降沿中断 */
static int nrf24_bus_init(const struct device *dev)
{
	const struct nrf24_config *cfg = get_cfg(dev);
	struct nrf24_data *d = get_data(dev);
	int ret;

	if (!spi_is_ready_dt(&cfg->bus)) {
		LOG_ERR("SPI bus not ready");
		return -ENODEV;
	}
	if (!gpio_is_ready_dt(&cfg->ce) || !gpio_is_ready_dt(&cfg->irq)) {
		LOG_ERR("CE/IRQ GPIO not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&cfg->ce, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		return ret;
	}
	ret = gpio_pin_configure_dt(&cfg->irq, GPIO_INPUT);
	if (ret < 0) {
		return ret;
	}

	gpio_init_callback(&d->irq_cb, nrf24_irq_handler, BIT(cfg->irq.pin));
	ret = gpio_add_callback(cfg->irq.port, &d->irq_cb);
	if (ret < 0) {
		return ret;
	}
	/* IRQ 为 active-low（见 overlay irq-gpios），转 active 即下降沿 */
	ret = gpio_pin_interrupt_configure_dt(&cfg->irq, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret < 0) {
		return ret;
	}
	return 0;
}

static uint8_t data_rate_bits(enum nrf24_data_rate r)
{
	switch (r) {
	case NRF24_RATE_250K:
		return BIT(RF_DR_LOW);
	case NRF24_RATE_2M:
		return BIT(RF_DR_HIGH);
	default:                  /* 1M：两位均清（修正源驱动 SetDataRate bug）*/
		return 0;
	}
}

static uint8_t tx_power_bits(enum nrf24_tx_power p)
{
	switch (p) {
	case NRF24_PWR_18DBM:
		return PWR_N_18DB;
	case NRF24_PWR_12DBM:
		return PWR_N_12DB;
	case NRF24_PWR_6DBM:
		return PWR_N_6DB;
	default:
		return PWR_N_0DB;
	}
}

static uint8_t addr_aw_bits(uint8_t aw)
{
	switch (aw) {
	case 3:
		return AW_3BYTES;
	case 4:
		return AW_4BYTES;
	default:
		return AW_5BYTES;
	}
}

/* 锁内：按 cfg 写入全部 RF 寄存器。prim_rx 控制 PRIM_RX 位。*/
static int apply_config_with(const struct device *dev, const struct nrf24_config *cfg, bool prim_rx)
{
	uint8_t aw = (cfg->address_width >= 3 && cfg->address_width <= 5) ? cfg->address_width : 5;
	int ret;

	/* 进入 powerdown */
	ret = reg_write_locked(dev, L01REG_CONFIG, 0);
	if (ret < 0) {
		return ret;
	}

	ret = reg_write_locked(dev, L01REG_SETUP_AW, addr_aw_bits(aw));
	if (ret < 0) {
		return ret;
	}

	ret = reg_write_locked(dev, L01REG_SETUP_RETR, (uint8_t)(cfg->ard | (cfg->arc & 0x0F)));
	if (ret < 0) {
		return ret;
	}

	ret = reg_write_locked(dev, L01REG_RF_CH, cfg->channel & 0x7F);
	if (ret < 0) {
		return ret;
	}

	uint8_t rf = data_rate_bits(parse_data_rate(cfg->data_rate)) |
		     tx_power_bits(parse_tx_power(cfg->tx_power));

	ret = reg_write_locked(dev, L01REG_RF_SETUP, rf);
	if (ret < 0) {
		return ret;
	}

	ret = reg_write_locked(dev, L01REG_ENAA, BIT(ENAA_P0));
	if (ret < 0) {
		return ret;
	}

	ret = reg_write_locked(dev, L01REG_EN_RXADDR, BIT(ERX_P0));
	if (ret < 0) {
		return ret;
	}

	/* CONFIG：CRC（源驱动 bug 修正——显式 CRCO）+ PWR_UP + PRIM_RX */
	uint8_t config = BIT(EN_CRC) | BIT(PWR_UP);

	if (parse_crc_mode(cfg->crc_mode) == NRF24_CRC_2_BYTE) {
		config |= BIT(CRCO);
	}
	if (prim_rx) {
		config |= BIT(PRIM_RX);
	}
	ret = reg_write_locked(dev, L01REG_CONFIG, config);
	if (ret < 0) {
		return ret;
	}

	/* 载荷模式 */
	if (parse_payload_mode(cfg->payload_mode) == NRF24_PAYLOAD_DYNAMIC) {
		ret = reg_write_locked(dev, L01REG_DYNPD, BIT(DPL_P0));
		if (ret < 0) {
			return ret;
		}
		ret = reg_write_locked(dev, L01REG_FEATURE, BIT(EN_DPL) | BIT(EN_ACK_PAY));
		if (ret < 0) {
			return ret;
		}
	} else {
		ret = reg_write_locked(dev, L01REG_DYNPD, 0);
		if (ret < 0) {
			return ret;
		}
		ret = reg_write_locked(dev, L01REG_FEATURE, 0);
		if (ret < 0) {
			return ret;
		}
		ret = reg_write_locked(dev, L01REG_RX_PW_P0, cfg->rx_payload_width & 0x3F);
		if (ret < 0) {
			return ret;
		}
	}

	/* 地址：TX_ADDR 与 RX_ADDR_P0 同值（PTX 用 P0 收 ACK）*/
	ret = reg_write_multi_locked(dev, L01REG_TX_ADDR, cfg->tx_addr, aw);
	if (ret < 0) {
		return ret;
	}
	ret = reg_write_multi_locked(dev, L01REG_RX_ADDR_P0, cfg->tx_addr, aw);
	if (ret < 0) {
		return ret;
	}

	/* 清 FIFO 与 IRQ */
	cmd_locked(dev, FLUSH_TX);
	cmd_locked(dev, FLUSH_RX);
	reg_write_locked(dev, L01REG_STATUS, IRQ_ALL);

	/* 等待 PWR_UP/晶振稳定（datasheet：1.5ms）*/
	k_msleep(2);
	return 0;
}

#define NRF24_CFG_INIT(idx)                                                     \
static const struct nrf24_config nrf24_cfg_##idx = {                            \
	.bus = SPI_DT_SPEC_INST_GET(idx, NRF24_SPI_OPERATION),                      \
	.ce  = GPIO_DT_SPEC_INST_GET(idx, ce_gpios),                                \
	.irq = GPIO_DT_SPEC_INST_GET(idx, irq_gpios),                               \
	.channel = DT_INST_PROP(idx, channel),                                      \
	.data_rate = DT_INST_PROP(idx, data_rate),                                  \
	.tx_power = DT_INST_PROP(idx, tx_power),                                    \
	.address_width = DT_INST_PROP(idx, address_width),                          \
	.payload_mode = DT_INST_PROP(idx, payload_mode),                            \
	.rx_payload_width = DT_INST_PROP(idx, rx_payload_width),                    \
	.crc_mode = DT_INST_PROP(idx, crc_mode),                                    \
	.ard = (uint8_t)ARD_US(DT_INST_PROP(idx, ard_us)),                          \
	.arc = DT_INST_PROP(idx, arc),                                              \
	.tx_addr = {                                                                \
		DT_INST_PROP_BY_IDX(idx, tx_address, 0),                            \
		DT_INST_PROP_BY_IDX(idx, tx_address, 1),                            \
		DT_INST_PROP_BY_IDX(idx, tx_address, 2),                            \
		DT_INST_PROP_BY_IDX(idx, tx_address, 3),                            \
		DT_INST_PROP_BY_IDX(idx, tx_address, 4),                            \
	},                                                                          \
};

/* 桩实现 —— Task 7-10 填充 */
/* —— 枚举到 DT 字符串的反向映射（configure 用临时 config 时桥接）—— */
static const char *data_rate_to_str(enum nrf24_data_rate r)
{
	switch (r) {
	case NRF24_RATE_250K:
		return "250k";
	case NRF24_RATE_2M:
		return "2m";
	default:
		return "1m";
	}
}

static const char *tx_power_to_str(enum nrf24_tx_power p)
{
	switch (p) {
	case NRF24_PWR_18DBM:
		return "-18dbm";
	case NRF24_PWR_12DBM:
		return "-12dbm";
	case NRF24_PWR_6DBM:
		return "-6dbm";
	default:
		return "0dbm";
	}
}

static const char *crc_mode_to_str(enum nrf24_crc_mode c)
{
	return (c == NRF24_CRC_1_BYTE) ? "1-byte" : "2-byte";
}

/* 锁内：切 PRIM_RX 位 + CE（含 PWR_UP 0→1 稳定等待）*/
static int set_mode_locked(const struct device *dev, enum nrf24_mode mode)
{
	const struct nrf24_config *cfg = get_cfg(dev);
	struct nrf24_data *d = get_data(dev);
	uint8_t config;
	bool was_powered;
	int ret = reg_read_locked(dev, L01REG_CONFIG, &config);

	if (ret < 0) {
		return ret;
	}
	was_powered = (config & BIT(PWR_UP)) != 0;

	config &= ~(BIT(PWR_UP) | BIT(PRIM_RX));
	switch (mode) {
	case NRF24_MODE_PRX:
		config |= BIT(PWR_UP) | BIT(PRIM_RX);
		break;
	case NRF24_MODE_PTX:
		config |= BIT(PWR_UP);
		break;
	case NRF24_MODE_STANDBY:
		config |= BIT(PWR_UP);
		break;
	case NRF24_MODE_POWER_DOWN:
		break;
	}
	ret = reg_write_locked(dev, L01REG_CONFIG, config);
	if (ret < 0) {
		return ret;
	}

	/* PWR_UP 0→1 需 ~1.5ms 晶振稳定（datasheet t pd2stdby）*/
	if (!was_powered && mode != NRF24_MODE_POWER_DOWN) {
		k_msleep(2);
	}

	gpio_pin_set_dt(&cfg->ce, (mode == NRF24_MODE_PRX) ? 1 : 0);
	d->mode = mode;
	return 0;
}

int nrf24_set_mode(const struct device *dev, enum nrf24_mode mode)
{
	struct nrf24_data *d = get_data(dev);
	int ret;

	k_mutex_lock(&d->lock, K_FOREVER);
	ret = set_mode_locked(dev, mode);
	k_mutex_unlock(&d->lock);
	return ret;
}

int nrf24_start_rx(const struct device *dev)
{
	return nrf24_set_mode(dev, NRF24_MODE_PRX);
}

bool nrf24_rx_ready(const struct device *dev)
{
	uint8_t status = 0;

	if (nrf24_read_reg(dev, L01REG_STATUS, &status) < 0) {
		return false;
	}
	return (status & BIT(RX_DR)) != 0;
}

int nrf24_send(const struct device *dev, const void *buf, size_t len, k_timeout_t timeout)
{
	struct nrf24_data *d = get_data(dev);
	uint8_t status = 0;
	int ret;

	if (len == 0 || len > NRF24_MAX_PAYLOAD) {
		return -EINVAL;
	}

	k_mutex_lock(&d->lock, K_FOREVER);

	/* 排空陈旧 sem */
	while (k_sem_take(&d->irq_sem, K_NO_WAIT) == 0) {
	}

	ret = set_mode_locked(dev, NRF24_MODE_PTX);
	if (ret < 0) {
		goto out;
	}

	cmd_locked(dev, FLUSH_TX);
	ret = write_tx_payload_locked(dev, buf, (uint8_t)len);
	if (ret < 0) {
		goto out;
	}

	/* CE 高电平 ≥10us 触发发送 */
	ce_set(dev, 1);
	k_busy_wait(15);

	ret = k_sem_take(&d->irq_sem, timeout);
	if (ret < 0) {
		ce_set(dev, 0);
		ret = -EAGAIN;
		goto out;
	}

	reg_read_locked(dev, L01REG_STATUS, &status);
	if (status & BIT(MAX_RT)) {
		cmd_locked(dev, FLUSH_TX);
		ret = -EIO;
	} else {
		ret = 0;
	}
	reg_write_locked(dev, L01REG_STATUS, IRQ_ALL);
	ce_set(dev, 0);

out:
	k_mutex_unlock(&d->lock);
	return ret;
}

int nrf24_recv(const struct device *dev, void *buf, size_t max_len, k_timeout_t timeout)
{
	struct nrf24_data *d = get_data(dev);
	const struct nrf24_config *cfg = get_cfg(dev);
	uint8_t status = 0;
	uint8_t plen = 0;
	int ret;

	k_mutex_lock(&d->lock, K_FOREVER);

	if (d->mode != NRF24_MODE_PRX) {
		ret = set_mode_locked(dev, NRF24_MODE_PRX);
		if (ret < 0) {
			goto out;
		}
	}

	reg_read_locked(dev, L01REG_STATUS, &status);
	if (!(status & BIT(RX_DR))) {
		while (k_sem_take(&d->irq_sem, K_NO_WAIT) == 0) {
		}
		ret = k_sem_take(&d->irq_sem, timeout);
		if (ret < 0) {
			ret = -EAGAIN;
			goto out;
		}
		reg_read_locked(dev, L01REG_STATUS, &status);
	}

	if (!(status & BIT(RX_DR))) {
		ret = -EIO;
		goto out;
	}

	if (parse_payload_mode(cfg->payload_mode) == NRF24_PAYLOAD_DYNAMIC) {
		ret = read_rx_pl_wid_locked(dev, &plen);
		if (ret < 0) {
			goto out;
		}
		if (plen > NRF24_MAX_PAYLOAD) {
			cmd_locked(dev, FLUSH_RX);
			ret = -EIO;
			goto out_clr;
		}
	} else {
		plen = (cfg->rx_payload_width <= NRF24_MAX_PAYLOAD) ? cfg->rx_payload_width
								    : NRF24_MAX_PAYLOAD;
	}

	if (plen > max_len) {
		plen = (uint8_t)max_len;
	}
	ret = read_rx_payload_locked(dev, buf, plen);
	if (ret < 0) {
		goto out_clr;
	}
	ret = plen;

out_clr:
	cmd_locked(dev, FLUSH_RX);
	reg_write_locked(dev, L01REG_STATUS, BIT(RX_DR));
out:
	k_mutex_unlock(&d->lock);
	return ret;
}

int nrf24_configure(const struct device *dev, const struct nrf24_cfg *cfg)
{
	struct nrf24_data *d = get_data(dev);
	const struct nrf24_config *dcfg = get_cfg(dev);
	struct nrf24_config tmp = *dcfg;
	int ret;

	if (cfg) {
		tmp.channel = cfg->channel;
		tmp.data_rate = data_rate_to_str(cfg->data_rate);
		tmp.tx_power = tx_power_to_str(cfg->tx_power);
		tmp.crc_mode = crc_mode_to_str(cfg->crc_mode);
		if (cfg->address_width >= 3 && cfg->address_width <= 5) {
			tmp.address_width = cfg->address_width;
		}
		if (cfg->tx_addr) {
			memcpy(tmp.tx_addr, cfg->tx_addr, tmp.address_width);
		}
	}

	k_mutex_lock(&d->lock, K_FOREVER);
	ret = apply_config_with(dev, &tmp, d->mode == NRF24_MODE_PRX);
	k_mutex_unlock(&d->lock);
	return ret;
}

int nrf24_read_reg(const struct device *dev, uint8_t reg, uint8_t *val)
{
	struct nrf24_data *d = get_data(dev);
	int ret;

	k_mutex_lock(&d->lock, K_FOREVER);
	ret = reg_read_locked(dev, reg, val);
	k_mutex_unlock(&d->lock);
	return ret;
}

int nrf24_write_reg(const struct device *dev, uint8_t reg, uint8_t val)
{
	struct nrf24_data *d = get_data(dev);
	int ret;

	k_mutex_lock(&d->lock, K_FOREVER);
	ret = reg_write_locked(dev, reg, val);
	k_mutex_unlock(&d->lock);
	return ret;
}

int nrf24_read_reg_multi(const struct device *dev, uint8_t reg, uint8_t *buf, size_t len)
{
	struct nrf24_data *d = get_data(dev);
	int ret;

	k_mutex_lock(&d->lock, K_FOREVER);
	ret = reg_read_multi_locked(dev, reg, buf, len);
	k_mutex_unlock(&d->lock);
	return ret;
}

int nrf24_write_reg_multi(const struct device *dev, uint8_t reg, const uint8_t *buf, size_t len)
{
	struct nrf24_data *d = get_data(dev);
	int ret;

	k_mutex_lock(&d->lock, K_FOREVER);
	ret = reg_write_multi_locked(dev, reg, buf, len);
	k_mutex_unlock(&d->lock);
	return ret;
}

static int nrf24_init(const struct device *dev)
{
	struct nrf24_data *d = get_data(dev);
	int ret;

	k_sem_init(&d->irq_sem, 0, 1);
	k_mutex_init(&d->lock);
	d->mode = NRF24_MODE_STANDBY;
	d->ready = false;

	ret = nrf24_bus_init(dev);
	if (ret < 0) {
		LOG_ERR("bus init failed: %d", ret);
		return ret;
	}

	k_mutex_lock(&d->lock, K_FOREVER);
	ret = apply_config_with(dev, get_cfg(dev), true);   /* 默认上电进入 PRX 待命 */
	if (ret < 0) {
		LOG_ERR("apply_config failed: %d", ret);
		k_mutex_unlock(&d->lock);
		return ret;
	}

	/* 自检：回读 SETUP_AW 验证 SPI 链路 */
	uint8_t aw_rb = 0;

	reg_read_locked(dev, L01REG_SETUP_AW, &aw_rb);
	LOG_INF("self-check SETUP_AW=0x%02x", aw_rb);

	d->ready = true;
	d->mode = NRF24_MODE_PRX;
	k_mutex_unlock(&d->lock);
	ce_set(dev, 1);   /* PRX：CE 保持高 */

	LOG_INF("nRF24L01P initialized");
	return 0;
}

#define NRF24_DEVICE(idx)                                        \
	NRF24_CFG_INIT(idx)                                      \
	static struct nrf24_data nrf24_data_##idx;               \
	DEVICE_DT_INST_DEFINE(idx, nrf24_init, NULL,             \
		&nrf24_data_##idx, &nrf24_cfg_##idx,                 \
		POST_KERNEL, CONFIG_NRF24L01P_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(NRF24_DEVICE)
