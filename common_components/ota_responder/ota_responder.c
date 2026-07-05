/*
 * ota_responder.c - responder-side ESP-NOW OTA for the worker node.
 *
 * Flow (stop-and-wait, in-order; see mesh_proto.h):
 *   gateway --MSG_OTA_BEGIN--> worker : esp_ota_begin() on the next slot
 *   gateway --MSG_OTA_DATA--->  worker : esp_ota_write() sequentially
 *           <--MSG_OTA_STATUS-- worker : OK(next_offset) / NEED(resend here)
 *   gateway --MSG_OTA_END--->   worker : esp_ota_end() + set_boot_partition()
 *           <--MSG_OTA_STATUS-- worker : DONE, then esp_restart()
 *
 * On the first boot of the new image the bootloader marks it PENDING_VERIFY
 * (rollback enabled). ota_responder_boot_check() runs a self-test and either
 * confirms the image (esp_ota_mark_app_valid_cancel_rollback) or rolls back
 * (esp_ota_mark_app_invalid_rollback_and_reboot). A crash/hang/power-loss before
 * the confirm call is auto-rolled-back by the bootloader on the next reset.
 *
 * The flash-commit path is the standard esp_ota_* API per the ESP-IDF OTA docs;
 * only the transport (ESP-NOW chunks) differs. See esp_now_ota_robustness.md.
 */
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_now.h"
#include "esp_wifi.h"

#include "mesh_proto.h"
#include "ota_responder.h"

static const char *TAG = "ota";

/* Flip to 1 and rebuild to ship a deliberately-failing self-test image, to
 * exercise the rollback path (esp_now_ota_robustness.md section 8). */
#define OTA_FORCE_SELFTEST_FAIL  0

#define OTA_TASK_STACK   8192
#define OTA_TASK_PRIO    4
#define OTA_QUEUE_DEPTH  8
#define OTA_FRAME_MAX    250          /* ESP_NOW_MAX_DATA_LEN */

typedef struct {
    uint8_t  src[6];
    uint16_t len;
    uint8_t  buf[OTA_FRAME_MAX];
} ota_evt_t;

static QueueHandle_t s_q;
static volatile int  s_pct = -1;      /* -1 idle, else 0..100 */

/* Session state - touched only by the OTA task. */
static esp_ota_handle_t       s_handle;
static const esp_partition_t *s_part;
static bool      s_active;
static uint32_t  s_session;
static uint32_t  s_written;
static uint32_t  s_total;

/* ----------------------------- self-test -------------------------------- */
static bool ota_self_test(void)
{
#if OTA_FORCE_SELFTEST_FAIL
    ESP_LOGE(TAG, "self-test FORCED FAIL (OTA_FORCE_SELFTEST_FAIL=1)");
    return false;
#else
    /* Minimal but real: the radio is up and we have a usable STA MAC. Extend
     * with app-specific checks (sensor present, link established, ...). */
    uint32_t ver;
    uint8_t  mac[6];
    if (esp_now_get_version(&ver) != ESP_OK) return false;
    if (esp_wifi_get_mac(WIFI_IF_STA, mac) != ESP_OK) return false;
    return true;
#endif
}

void ota_responder_boot_check(void)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    ESP_LOGI(TAG, "running from '%s' @0x%08" PRIx32 " (size 0x%" PRIx32 ")",
             run->label, run->address, run->size);

    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(run, &st) != ESP_OK) return;
    if (st != ESP_OTA_IMG_PENDING_VERIFY) return;   /* normal (non-OTA) boot */

    ESP_LOGW(TAG, "image PENDING_VERIFY (first boot after OTA) -> self-test");
    if (ota_self_test()) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "self-test OK -> image VALID, rollback cancelled");
    } else {
        ESP_LOGE(TAG, "self-test FAILED -> rolling back to previous image");
        esp_ota_mark_app_invalid_rollback_and_reboot();   /* does not return */
    }
}

/* ----------------------------- replies ---------------------------------- */
static void ota_send_status(const uint8_t *dst, uint8_t state, int32_t err, uint32_t next_off)
{
    if (!esp_now_is_peer_exist(dst)) {
        esp_now_peer_info_t p = {0};
        memcpy(p.peer_addr, dst, 6);
        p.channel = MESH_WIFI_CHANNEL;
        p.ifidx   = WIFI_IF_STA;
        p.encrypt = false;
        if (esp_now_add_peer(&p) != ESP_OK) {
            ESP_LOGW(TAG, "cannot add peer for status reply");
            return;
        }
    }
    mesh_ota_status_t s = {0};
    s.hdr.magic   = MESH_PROTO_MAGIC;
    s.hdr.version = MESH_PROTO_VER;
    s.hdr.type    = MSG_OTA_STATUS;
    esp_wifi_get_mac(WIFI_IF_STA, s.hdr.src_mac);
    s.session_id  = s_session;
    s.state       = state;
    s.err         = err;
    s.next_offset = next_off;
    esp_now_send(dst, (const uint8_t *)&s, sizeof(s));
}

/* ----------------------------- handlers --------------------------------- */
static void handle_begin(const uint8_t *src, const mesh_ota_begin_t *b)
{
    /* Confirm the running app before starting a new update, otherwise
     * esp_ota_begin() returns ESP_ERR_OTA_ROLLBACK_INVALID_STATE while the
     * image is still PENDING_VERIFY (OTA docs / robustness doc section 4).
     * Only meaningful when running from an OTA slot - calling it from the
     * factory partition just logs "Running firmware is factory", so skip it. */
    const esp_partition_t *run = esp_ota_get_running_partition();
    if (run && run->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN
            && run->subtype <  ESP_PARTITION_SUBTYPE_APP_OTA_MAX) {
        esp_ota_mark_app_valid_cancel_rollback();
    }

    if (s_active) {                 /* a prior session never finished */
        esp_ota_abort(s_handle);
        s_active = false;
    }

    s_session = b->session_id;
    s_total   = b->total_size;
    s_written = 0;

    s_part = esp_ota_get_next_update_partition(NULL);
    if (!s_part) {
        ota_send_status(src, OTA_ST_ERR, ESP_ERR_NOT_FOUND, 0);
        return;
    }
    ESP_LOGI(TAG, "begin: session %" PRIu32 " ver '%.*s' total %" PRIu32 "B -> '%s'",
             b->session_id, OTA_FW_VER_LEN, b->fw_version, b->total_size, s_part->label);

    /* OTA_WITH_SEQUENTIAL_WRITES erases on the fly as we stream in order, which
     * is exactly our chunking model (no esp_ota_write_with_offset()). */
    esp_err_t e = esp_ota_begin(s_part, OTA_WITH_SEQUENTIAL_WRITES, &s_handle);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(e));
        ota_send_status(src, OTA_ST_ERR, e, 0);
        return;
    }
    s_active = true;
    s_pct = 0;
    ota_send_status(src, OTA_ST_READY, 0, 0);
}

static void handle_data(const uint8_t *src, const mesh_ota_data_t *d, int frame_len)
{
    if (!s_active || d->session_id != s_session) {
        ota_send_status(src, OTA_ST_ERR, ESP_ERR_INVALID_STATE, s_written);
        return;
    }
    if (d->len > OTA_CHUNK_MAX ||
        frame_len < (int)(offsetof(mesh_ota_data_t, data) + d->len)) {
        ota_send_status(src, OTA_ST_ERR, ESP_ERR_INVALID_SIZE, s_written);
        return;
    }
    if (d->offset != s_written) {       /* lost / duplicate / out of order */
        ota_send_status(src, OTA_ST_NEED, 0, s_written);
        return;
    }
    esp_err_t e = esp_ota_write(s_handle, d->data, d->len);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write @%" PRIu32 ": %s", s_written, esp_err_to_name(e));
        esp_ota_abort(s_handle);
        s_active = false;
        s_pct = -1;
        ota_send_status(src, OTA_ST_ERR, e, s_written);
        return;
    }
    s_written += d->len;
    s_pct = s_total ? (int)((uint64_t)s_written * 100 / s_total) : 50;
    ota_send_status(src, OTA_ST_OK, 0, s_written);
}

static void handle_end(const uint8_t *src, const mesh_ota_end_t *m)
{
    if (!s_active || m->session_id != s_session) {
        ota_send_status(src, OTA_ST_ERR, ESP_ERR_INVALID_STATE, s_written);
        return;
    }
    if (m->total_size && m->total_size != s_written) {  /* incomplete -> resume */
        ESP_LOGW(TAG, "end size mismatch: have %" PRIu32 ", expected %" PRIu32,
                 s_written, m->total_size);
        ota_send_status(src, OTA_ST_NEED, 0, s_written);
        return;
    }

    /* esp_ota_end() validates the image (magic byte, checksum/SHA-256, and
     * signature under secure boot). On failure the boot slot is NOT changed,
     * so the device keeps running the old app. */
    esp_err_t e = esp_ota_end(s_handle);
    s_active = false;
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(e));   /* e.g. VALIDATE_FAILED */
        s_pct = -1;
        ota_send_status(src, OTA_ST_ERR, e, s_written);
        return;
    }
    e = esp_ota_set_boot_partition(s_part);
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(e));
        s_pct = -1;
        ota_send_status(src, OTA_ST_ERR, e, s_written);
        return;
    }
    s_pct = 100;
    ESP_LOGI(TAG, "committed %" PRIu32 " bytes -> rebooting into new image", s_written);
    ota_send_status(src, OTA_ST_DONE, 0, s_written);
    vTaskDelay(pdMS_TO_TICKS(300));   /* let the DONE frame + log flush */
    esp_restart();
}

/* ----------------------------- task ------------------------------------- */
static void ota_task(void *arg)
{
    (void)arg;
    ota_evt_t e;
    for (;;) {
        if (xQueueReceive(s_q, &e, portMAX_DELAY) != pdTRUE) continue;
        const mesh_hdr_t *h = (const mesh_hdr_t *)e.buf;
        switch (h->type) {
        case MSG_OTA_BEGIN:
            if (e.len >= (int)sizeof(mesh_ota_begin_t))
                handle_begin(e.src, (const mesh_ota_begin_t *)e.buf);
            break;
        case MSG_OTA_DATA:
            if (e.len >= (int)offsetof(mesh_ota_data_t, data))
                handle_data(e.src, (const mesh_ota_data_t *)e.buf, e.len);
            break;
        case MSG_OTA_END:
            if (e.len >= (int)sizeof(mesh_ota_end_t))
                handle_end(e.src, (const mesh_ota_end_t *)e.buf);
            break;
        default:
            break;
        }
    }
}

void ota_responder_start(void)
{
    s_q = xQueueCreate(OTA_QUEUE_DEPTH, sizeof(ota_evt_t));
    if (!s_q) {
        ESP_LOGE(TAG, "OTA queue alloc failed");
        return;
    }
    if (xTaskCreate(ota_task, "ota_rx", OTA_TASK_STACK, NULL, OTA_TASK_PRIO, NULL) != pdPASS)
        ESP_LOGE(TAG, "OTA task create failed");
    else
        ESP_LOGI(TAG, "OTA responder ready");
}

bool ota_responder_rx(const uint8_t *src_mac, const uint8_t *data, int len)
{
    if (len < (int)sizeof(mesh_hdr_t)) return false;
    const mesh_hdr_t *h = (const mesh_hdr_t *)data;
    if (h->magic != MESH_PROTO_MAGIC || h->version != MESH_PROTO_VER) return false;
    if (h->type != MSG_OTA_BEGIN && h->type != MSG_OTA_DATA && h->type != MSG_OTA_END)
        return false;                 /* not an OTA frame; let caller handle it */

    /* From here it IS ours: consume it even if we have to drop it. */
    if (!s_q || len > OTA_FRAME_MAX) return true;

    ota_evt_t e;
    memcpy(e.src, src_mac, 6);
    e.len = (uint16_t)len;
    memcpy(e.buf, data, len);
    if (xQueueSend(s_q, &e, 0) != pdTRUE)
        ESP_LOGW(TAG, "OTA queue full, chunk dropped (sender will resend)");
    return true;
}

int ota_responder_progress_pct(void)
{
    return s_pct;
}
