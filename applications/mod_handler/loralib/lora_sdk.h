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
 * Scanner data (merged frame, 20-byte payload)
 *
 * Merged frame payload layout (Data field of unified frame):
 *   [0]    type       0x01 (LORA_DATA_HANDLER)
 *   [1]    flags      validity bitmask
 *   [2-3]  overbreak  int16_t BE
 *   [4-7]  laser      uint32_t BE
 *   [8-11] coord_x    int32_t BE
 *   [12-15]coord_y    int32_t BE
 *   [16-19]coord_z    int32_t BE
 * ---------------------------------------------------------------- */

#define LORA_SCANNER_F_OVERBREAK  0x01
#define LORA_SCANNER_F_LASER      0x02
#define LORA_SCANNER_F_COORD_Z    0x04
#define LORA_SCANNER_F_COORD_XY   0x08

#define LORA_SCANNER_FRAME_SIZE   20

typedef struct {
    uint8_t overbreak_valid : 1;
    uint8_t laser_valid     : 1;
    uint8_t coord_z_valid   : 1;
    uint8_t coord_xy_valid  : 1;
    uint8_t reserved        : 4;
    int16_t  overbreak;
    uint32_t laser;
    int32_t  coord_x;
    int32_t  coord_y;
    int32_t  coord_z;
} lora_scanner_data_t;

/* Byte-order helpers (static inline, no external dependency) */
static inline void lora_put_be16(uint8_t *buf, uint16_t val)
{
    buf[0] = (uint8_t)(val >> 8);
    buf[1] = (uint8_t)(val);
}

static inline void lora_put_be32(uint8_t *buf, uint32_t val)
{
    buf[0] = (uint8_t)(val >> 24);
    buf[1] = (uint8_t)(val >> 16);
    buf[2] = (uint8_t)(val >> 8);
    buf[3] = (uint8_t)(val);
}

static inline uint16_t lora_get_be16(const uint8_t *buf)
{
    return (uint16_t)buf[0] << 8 | buf[1];
}

static inline uint32_t lora_get_be32(const uint8_t *buf)
{
    return (uint32_t)buf[0] << 24 | (uint32_t)buf[1] << 16 |
           (uint32_t)buf[2] << 8  | (uint32_t)buf[3];
}

/* Parse merged scanner frame payload into struct.
 * 'payload' points to the Data field (including type byte).
 * 'len' is the Data field length.
 * Returns 0 on success, -1 if payload too short or wrong type. */
static inline int lora_scanner_parse(const uint8_t *payload, uint16_t len,
                                     lora_scanner_data_t *out)
{
    if (len < LORA_SCANNER_FRAME_SIZE || payload[0] != 0x01) return -1;
    uint8_t flags = payload[1];
    out->overbreak_valid = (flags >> 0) & 1;
    out->laser_valid     = (flags >> 1) & 1;
    out->coord_z_valid   = (flags >> 2) & 1;
    out->coord_xy_valid  = (flags >> 3) & 1;
    out->overbreak = (int16_t)lora_get_be16(payload + 2);
    out->laser     = lora_get_be32(payload + 4);
    out->coord_x   = (int32_t)lora_get_be32(payload + 8);
    out->coord_y   = (int32_t)lora_get_be32(payload + 12);
    out->coord_z   = (int32_t)lora_get_be32(payload + 16);
    return 0;
}

/* Pack scanner struct into merged frame payload buffer.
 * 'buf' must be at least LORA_SCANNER_FRAME_SIZE bytes.
 * Returns number of bytes written (20), or -1 if buffer too small. */
static inline int lora_scanner_pack(uint8_t *buf, size_t size,
                                    const lora_scanner_data_t *s)
{
    if (size < LORA_SCANNER_FRAME_SIZE) return -1;
    buf[0] = 0x01; /* LORA_DATA_HANDLER */
    buf[1] = (s->overbreak_valid << 0) | (s->laser_valid << 1) |
             (s->coord_z_valid << 2)   | (s->coord_xy_valid << 3);
    lora_put_be16(buf + 2, (uint16_t)s->overbreak);
    lora_put_be32(buf + 4, s->laser);
    lora_put_be32(buf + 8, (uint32_t)s->coord_x);
    lora_put_be32(buf + 12, (uint32_t)s->coord_y);
    lora_put_be32(buf + 16, (uint32_t)s->coord_z);
    return LORA_SCANNER_FRAME_SIZE;
}

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
