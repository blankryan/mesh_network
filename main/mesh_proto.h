/*
 * mesh_proto.h - shared ESP-NOW packet format for the worker <-> gateway link.
 * Keep this file IDENTICAL on both nodes. All structs are packed and well
 * under ESP_NOW_MAX_DATA_LEN (250 B), so they remain ESP-NOW v1.0 compatible.
 */
#pragma once
#include <stdint.h>
#include <stdio.h>

/* Both nodes MUST sit on the same Wi-Fi channel (no router in this setup). */
#define MESH_WIFI_CHANNEL   1

#define MESH_PROTO_MAGIC    0x4E4D   /* 'M''N' */
#define MESH_PROTO_VER      1

typedef enum {
    MSG_TELEMETRY  = 1,   /* worker  -> gateway : sensor data                 */
    MSG_ACK        = 2,   /* gateway -> worker  : announce gateway MAC        */
    /* OTA over ESP-NOW (leg 2 of esp_now_ota_robustness.md): gateway is the
     * initiator/sender, worker is the responder that writes esp_ota_*.        */
    MSG_OTA_BEGIN  = 3,   /* gateway -> worker  : start a firmware push        */
    MSG_OTA_DATA   = 4,   /* gateway -> worker  : one firmware chunk           */
    MSG_OTA_END    = 5,   /* gateway -> worker  : finalize / commit            */
    MSG_OTA_STATUS = 6,   /* worker  -> gateway : progress / ack / nack / done */
} mesh_msg_type_t;

typedef struct __attribute__((packed)) {
    uint16_t magic;       /* MESH_PROTO_MAGIC               */
    uint8_t  version;     /* MESH_PROTO_VER                 */
    uint8_t  type;        /* mesh_msg_type_t                */
    uint8_t  src_mac[6];  /* sender STA MAC                 */
    uint16_t seq;         /* sender sequence counter        */
} mesh_hdr_t;

typedef struct __attribute__((packed)) {
    mesh_hdr_t hdr;
    int16_t    temp_c_x10;  /* temperature  * 10  (253 = 25.3 C) */
    uint16_t   rh_x10;      /* rel. humidity * 10 (601 = 60.1 %) */
    uint32_t   uptime_s;    /* seconds since boot                */
} mesh_telemetry_t;

typedef struct __attribute__((packed)) {
    mesh_hdr_t hdr;
    uint8_t    gateway_mac[6];
} mesh_ack_t;

/* ----------------------------- OTA messages ------------------------------ *
 * Stop-and-wait, in-order chunking so the responder uses sequential
 * esp_ota_write() only (no esp_ota_write_with_offset(), no alignment quirks).
 * Out-of-order or lost chunks are recovered by the responder NACKing with the
 * next byte offset it expects (MSG_OTA_STATUS / OTA_ST_NEED), and the sender
 * resuming from there. Every struct stays <= ESP_NOW_MAX_DATA_LEN (250 B).   */

#define OTA_FW_VER_LEN   24
#define OTA_CHUNK_MAX    192          /* mesh_ota_data_t -> 214 B on the wire  */

typedef struct __attribute__((packed)) {
    mesh_hdr_t hdr;
    uint32_t   session_id;            /* identifies this OTA push              */
    uint32_t   total_size;            /* image bytes, 0 = unknown              */
    uint16_t   chunk_size;            /* sender's chunk size (<= OTA_CHUNK_MAX)*/
    char       fw_version[OTA_FW_VER_LEN]; /* informational (esp_app_desc)     */
} mesh_ota_begin_t;

typedef struct __attribute__((packed)) {
    mesh_hdr_t hdr;
    uint32_t   session_id;
    uint32_t   offset;                /* byte offset of this chunk             */
    uint16_t   len;                   /* valid bytes in data[]                 */
    uint8_t    data[OTA_CHUNK_MAX];   /* only the first `len` bytes are sent    */
} mesh_ota_data_t;

typedef struct __attribute__((packed)) {
    mesh_hdr_t hdr;
    uint32_t   session_id;
    uint32_t   total_size;            /* full image length the sender wrote     */
} mesh_ota_end_t;

typedef enum {
    OTA_ST_READY = 0,   /* begin accepted; send data starting at next_offset    */
    OTA_ST_OK    = 1,   /* chunk written; next_offset advanced                  */
    OTA_ST_NEED  = 2,   /* out-of-order/duplicate; (re)send from next_offset    */
    OTA_ST_DONE  = 3,   /* image committed; responder is rebooting              */
    OTA_ST_ERR   = 4,   /* fatal; see err (esp_err_t)                           */
} mesh_ota_state_t;

typedef struct __attribute__((packed)) {
    mesh_hdr_t hdr;
    uint32_t   session_id;
    uint8_t    state;                 /* mesh_ota_state_t                       */
    int32_t    err;                   /* esp_err_t when state == OTA_ST_ERR     */
    uint32_t   next_offset;           /* byte offset the responder expects next */
} mesh_ota_status_t;

static inline void mac_to_str(const uint8_t *m, char *out /* >=18 */)
{
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
             m[0], m[1], m[2], m[3], m[4], m[5]);
}
