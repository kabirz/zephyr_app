/*
 * Copyright (c) 2026 Kabirz.
 * SPDX-License-Identifier: Apache-2.0
 *
 * lora_sdk.h — LoRa Gateway SDK Public API
 *
 * TCP data streaming + UDP device discovery/configuration.
 * No Win32 GUI dependency. All notifications via callbacks.
 *
 * Thread safety: All callbacks fire from background worker threads.
 * Caller must marshal to UI thread if needed.
 */

#ifndef LORA_SDK_H
#define LORA_SDK_H

#include <stdint.h>
#include <stddef.h>

#define LORA_SDK_API __declspec(dllexport)

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------
 * Opaque handle
 * ---------------------------------------------------------------- */
typedef struct lora_sdk lora_sdk_t;

/* ----------------------------------------------------------------
 * Connection state
 * ---------------------------------------------------------------- */
enum lora_sdk_conn_state {
    LORA_SDK_CONN_DISCONNECTED = 0,
    LORA_SDK_CONN_CONNECTING   = 1,
    LORA_SDK_CONN_CONNECTED    = 2,
};

/* ----------------------------------------------------------------
 * SDK callback set — protocol events only
 *
 * All callbacks fire from background threads.  Callbacks MUST NOT
 * call SDK functions that perform synchronous I/O (connect,
 * disconnect, send) to avoid deadlocks.
 * ---------------------------------------------------------------- */
typedef struct {
    /* Connection state changed */
    void (*on_conn_state)(void *ud, enum lora_sdk_conn_state state);

    /* Parsed frame received. 'payload' includes the type byte.
     * payload[0] is the data type (0x01=HANDLER, 0x02=TEST, 0x03=RSSI).
     * Valid only during the callback. */
    void (*on_frame)(void *ud, uint32_t nid,
                     const uint8_t *payload, uint16_t payload_len);

    /* Device found during UDP search */
    void (*on_device_found)(void *ud, const char *mac,
                            const char *device_name, const char *sw_version,
                            const char *from_ip);

    /* Raw AT command response via UDP */
    void (*on_at_response)(void *ud, const char *at_response);

    /* Network parameters received (NETDEV query result) */
    void (*on_net_params)(void *ud, const char *ip,
                          const char *mask, const char *gateway);

    /* General log message */
    void (*on_log)(void *ud, const char *message);

    /* Hex dump of sent/received raw bytes */
    void (*on_hex_dump)(void *ud, const char *prefix,
                        const uint8_t *data, int len);

    /* Error notification */
    void (*on_error)(void *ud, const char *message);

} lora_sdk_callbacks_t;

/* ----------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------- */

LORA_SDK_API lora_sdk_t *lora_sdk_init(const lora_sdk_callbacks_t *callbacks,
                                        void *user_data);
LORA_SDK_API void lora_sdk_cleanup(lora_sdk_t *sdk);

/* ----------------------------------------------------------------
 * TCP operations
 * ---------------------------------------------------------------- */

LORA_SDK_API void lora_sdk_connect(lora_sdk_t *sdk, const char *ip, int port);
LORA_SDK_API void lora_sdk_disconnect(lora_sdk_t *sdk);
LORA_SDK_API enum lora_sdk_conn_state lora_sdk_conn_state(lora_sdk_t *sdk);

LORA_SDK_API void lora_sdk_send_frame(lora_sdk_t *sdk, uint32_t nid,
                                       const uint8_t *data, uint16_t data_len);

LORA_SDK_API void lora_sdk_send_rssi_response(lora_sdk_t *sdk, uint32_t nid,
                                                uint8_t snr_raw,
                                                uint8_t rssi_raw,
                                                uint8_t test_flag);

/* ----------------------------------------------------------------
 * UDP operations
 * ---------------------------------------------------------------- */

LORA_SDK_API void lora_sdk_search_devices(lora_sdk_t *sdk);
LORA_SDK_API void lora_sdk_get_net_params(lora_sdk_t *sdk);
LORA_SDK_API void lora_sdk_send_at(lora_sdk_t *sdk, const char *at_cmd);

/* ----------------------------------------------------------------
 * Utility
 * ---------------------------------------------------------------- */

LORA_SDK_API int lora_sdk_build_frame(uint8_t *out, size_t out_size,
                                       uint32_t nid,
                                       const uint8_t *data,
                                       uint16_t data_len);

#ifdef __cplusplus
}
#endif

#endif /* LORA_SDK_H */
