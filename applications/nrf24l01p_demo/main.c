#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <string.h>
#include "nrf24l01p.h"

LOG_MODULE_REGISTER(nrf24_demo, CONFIG_LOG_DEFAULT_LEVEL);

static const struct device *const nrf24 = DEVICE_DT_GET(DT_NODELABEL(nrf24));

/* RX 角色: 中断驱动 msgq 接收 */
K_MSGQ_DEFINE(demo_rx_msgq, sizeof(struct nrf24_frame), 8, 4);

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

		struct nrf24_tx_result result;
		int ret = nrf24_send(nrf24, payload, sizeof(payload), K_MSEC(100), &result);

		if (ret == 0) {
			LOG_INF("TX seq=%u acked=%d %ums retrans=%d",
				payload[0], result.acked, result.elapsed_ms, result.retransmits);
		} else {
			LOG_WRN("TX seq=%u failed: %d acked=%d retrans=%d",
				payload[0], ret, result.acked, result.retransmits);
		}
		k_sleep(K_SECONDS(1));
	}
}
#else
static void run_rx(void)
{
	int ret = nrf24_start_rx(nrf24);

	if (ret < 0) {
		LOG_ERR("start_rx failed: %d", ret);
		return;
	}
	/* 注册 msgq: IRQ 线程收到帧后投递到这里 */
	nrf24_add_rx_msgq(nrf24, &demo_rx_msgq);
	LOG_INF("RX waiting (irq-driven msgq)...");

	struct nrf24_frame frame;

	while (1) {
		ret = k_msgq_get(&demo_rx_msgq, &frame, K_FOREVER);
		if (ret == 0 && frame.len > 0) {
			LOG_INF("RX len=%u: [%02x %02x %02x %02x]",
				frame.len, frame.data[0], frame.data[1],
				frame.data[2], frame.data[3]);
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
