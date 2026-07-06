/** @file nrf24l01p.h
 *  @brief Nordic nRF24L01+ Zephyr 驱动公共 API。
 */
#ifndef NRF24L01P_H
#define NRF24L01P_H

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum nrf24_mode {
	NRF24_MODE_PTX,
	NRF24_MODE_PRX,
	NRF24_MODE_STANDBY,
	NRF24_MODE_POWER_DOWN,
};

enum nrf24_data_rate {
	NRF24_RATE_250K,
	NRF24_RATE_1M,
	NRF24_RATE_2M,
};

enum nrf24_tx_power {
	NRF24_PWR_18DBM,
	NRF24_PWR_12DBM,
	NRF24_PWR_6DBM,
	NRF24_PWR_0DBM,
};

enum nrf24_payload_mode {
	NRF24_PAYLOAD_DYNAMIC,
	NRF24_PAYLOAD_FIXED,
};

enum nrf24_crc_mode {
	NRF24_CRC_1_BYTE,
	NRF24_CRC_2_BYTE,
};

/** 运行时配置：仅覆盖高频可变参数的子集。
 *  payload_mode / rx_payload_width / ard / arc 等仅在 init 时从 DT 应用。
 *  传 NULL 表示按当前 DT 参数重新应用。
 */
struct nrf24_cfg {
	uint8_t channel;
	enum nrf24_data_rate data_rate;
	enum nrf24_tx_power tx_power;
	enum nrf24_crc_mode crc_mode;
	uint8_t address_width;          /* 3/4/5，0 表示不改 */
	const uint8_t *tx_addr;         /* 可空，长度须 = address_width */
};

/** 应用/重新应用配置。cfg=NULL 时按 DT 参数重新应用。*/
int nrf24_configure(const struct device *dev, const struct nrf24_cfg *cfg);

/** 阻塞发送一帧。len ∈ [1,32]。成功返回 0。*/
int nrf24_send(const struct device *dev, const void *buf, size_t len, k_timeout_t timeout);

/** 阻塞/超时接收一帧。返回载荷字节数（>0），负值为错误码。*/
int nrf24_recv(const struct device *dev, void *buf, size_t max_len, k_timeout_t timeout);

/** 切换 RF 模式。*/
int nrf24_set_mode(const struct device *dev, enum nrf24_mode mode);

/** 便捷：进入 PRX 并拉高 CE 开始接收。*/
int nrf24_start_rx(const struct device *dev);

/** 非阻塞查询是否收到数据（读 STATUS.RX_DR）。*/
bool nrf24_rx_ready(const struct device *dev);

/* —— 底层寄存器入口（power user）—— */
int nrf24_read_reg(const struct device *dev, uint8_t reg, uint8_t *val);
int nrf24_write_reg(const struct device *dev, uint8_t reg, uint8_t val);
int nrf24_read_reg_multi(const struct device *dev, uint8_t reg, uint8_t *buf, size_t len);
int nrf24_write_reg_multi(const struct device *dev, uint8_t reg, const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* NRF24L01P_H */
