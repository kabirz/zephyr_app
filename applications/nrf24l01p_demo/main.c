#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <string.h>
#include "nrf24l01p.h"

LOG_MODULE_REGISTER(nrf24_demo, CONFIG_LOG_DEFAULT_LEVEL);

static const struct device *const nrf24 = DEVICE_DT_GET(DT_NODELABEL(nrf24));

#ifdef CONFIG_NRF24L01P_DEMO_ROLE_TX
static void run_tx(void)
{
	uint8_t seq = 0;
	uint8_t payload[4];

	while (1) {
		payload[0] = seq++;
		payload[1] = 0xA5;
		payload[2] = 0x5A;
		payload[3] = (uint8_t)(0xFF - payload[0]);

		int ret = nrf24_send(nrf24, payload, sizeof(payload), K_MSEC(100));

		if (ret == 0) {
			LOG_INF("TX seq=%u ok", payload[0]);
		} else {
			LOG_WRN("TX seq=%u failed: %d", payload[0], ret);
		}
		k_sleep(K_SECONDS(1));
	}
}
#else
static void run_rx(void)
{
	uint8_t rbuf[32] = {0};
	int ret = nrf24_start_rx(nrf24);

	if (ret < 0) {
		LOG_ERR("start_rx failed: %d", ret);
		return;
	}

	while (1) {
		int n = nrf24_recv(nrf24, rbuf, sizeof(rbuf), K_SECONDS(5));

		if (n > 0) {
			LOG_INF("RX len=%d: [%02x %02x %02x %02x]",
				n, rbuf[0], rbuf[1], rbuf[2], rbuf[3]);
		} else if (n == -EAGAIN) {
			LOG_INF("RX timeout");
		} else {
			LOG_WRN("RX error: %d", n);
		}
	}
}
#endif

int main(void)
{
	if (!device_is_ready(nrf24)) {
		LOG_ERR("nRF24L01P device not ready");
		return 0;
	}
	LOG_INF("nRF24L01P ready, role=%s",
		IS_ENABLED(CONFIG_NRF24L01P_DEMO_ROLE_TX) ? "TX" : "RX");

#ifdef CONFIG_NRF24L01P_DEMO_ROLE_TX
	run_tx();
#else
	run_rx();
#endif
	return 0;
}
