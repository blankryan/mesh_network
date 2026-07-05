/*
 * ota_sender.c - initiator/sender-side ESP-NOW OTA for the gateway.
 *
 * Push core (stop-and-wait): for each frame we send, the worker replies with a
 * MSG_OTA_STATUS. We advance on OK, resume from next_offset on NEED (loss /
 * out-of-order), retry on timeout, and abort on ERR. This drives exactly the
 * loop ota_responder.c implements on the worker.
 *
 * Image source (Kconfig):
 *   LOOPBACK - read the gateway's own running app image (esp_image_get_metadata
 *              for the true length + esp_partition_read). No AP/server needed.
 *   HTTP     - leg 1: connect Wi-Fi STA, download into a flash staging slot;
 *              then return to the ESP-NOW channel and push (leg 2). The two legs
 *              are sequential because joining an AP moves the STA off channel 1.
 *
 * See esp_now_ota_robustness.md (two-leg model) and esp_now_mesh_architecture.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_now.h"
#include "esp_wifi.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"          /* esp_app_desc_t */
#include "esp_partition.h"
#include "esp_image_format.h"      /* esp_image_get_metadata, esp_image_metadata_t */
#include "driver/gpio.h"

#if CONFIG_GATEWAY_OTA_SRC_HTTP
#include <errno.h>
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "freertos/event_groups.h"
#endif

#include "mesh_proto.h"
#include "ota_sender.h"

static const char *TAG = "ota_tx";

#define OTA_TRIGGER_GPIO       0      /* BOOT button on most ESP32-S3 devkits */
#define OTA_STATUS_TIMEOUT_MS  600
#define OTA_MAX_RETRIES        10
#define OTA_TASK_STACK         8192
#define OTA_TASK_PRIO          4
#define OTA_STATUS_Q_DEPTH     4

static QueueHandle_t  s_status_q;
static uint8_t        s_my_mac[6];
static volatile uint8_t s_target[6];
static volatile bool  s_have_target;
static volatile bool  s_trigger;
static volatile int   s_pct = -1;
static uint32_t       s_session_ctr;

/* ----------------------------- tx helpers ------------------------------- */
static void ensure_peer(const uint8_t *dst)
{
    if (esp_now_is_peer_exist(dst)) return;
    esp_now_peer_info_t p = {0};
    memcpy(p.peer_addr, dst, 6);
    p.channel = MESH_WIFI_CHANNEL;
    p.ifidx   = WIFI_IF_STA;
    p.encrypt = false;
    esp_now_add_peer(&p);
}

static inline uint32_t ms_now(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* Wait for a status frame for this session, ignoring stale ones, until timeout. */
static bool wait_status(mesh_ota_status_t *st, uint32_t session, uint32_t timeout_ms)
{
    uint32_t deadline = ms_now() + timeout_ms;
    for (;;) {
        uint32_t now = ms_now();
        if (now >= deadline) return false;
        if (xQueueReceive(s_status_q, st, pdMS_TO_TICKS(deadline - now)) != pdTRUE)
            return false;
        if (st->session_id == session) return true;
        /* stale session reply - keep waiting */
    }
}

static void fill_hdr(mesh_hdr_t *h, uint8_t type)
{
    h->magic   = MESH_PROTO_MAGIC;
    h->version = MESH_PROTO_VER;
    h->type    = type;
    memcpy(h->src_mac, s_my_mac, 6);
    h->seq     = 0;
}

static bool do_begin(const uint8_t *dst, uint32_t session, uint32_t total,
                     const char *ver, uint32_t *off)
{
    mesh_ota_begin_t b = {0};
    fill_hdr(&b.hdr, MSG_OTA_BEGIN);
    b.session_id = session;
    b.total_size = total;
    b.chunk_size = OTA_CHUNK_MAX;
    snprintf(b.fw_version, OTA_FW_VER_LEN, "%s", ver);

    for (int t = 0; t < OTA_MAX_RETRIES; t++) {
        esp_now_send(dst, (const uint8_t *)&b, sizeof(b));
        mesh_ota_status_t st;
        if (wait_status(&st, session, OTA_STATUS_TIMEOUT_MS)) {
            if (st.state == OTA_ST_READY || st.state == OTA_ST_NEED) { *off = st.next_offset; return true; }
            if (st.state == OTA_ST_ERR) {
                ESP_LOGE(TAG, "begin rejected: err=%d", (int)st.err);
                return false;
            }
        }
    }
    ESP_LOGE(TAG, "begin: no READY after %d tries", OTA_MAX_RETRIES);
    return false;
}

static bool do_data(const uint8_t *dst, uint32_t session,
                    const esp_partition_t *src, uint32_t total, uint32_t *off)
{
    uint32_t cur = *off;
    uint32_t n = total - cur;
    if (n > OTA_CHUNK_MAX) n = OTA_CHUNK_MAX;

    mesh_ota_data_t d = {0};
    fill_hdr(&d.hdr, MSG_OTA_DATA);
    d.session_id = session;
    d.offset     = cur;
    d.len        = (uint16_t)n;
    if (esp_partition_read(src, cur, d.data, n) != ESP_OK) {
        ESP_LOGE(TAG, "partition read @%" PRIu32 " failed", cur);
        return false;
    }
    size_t flen = offsetof(mesh_ota_data_t, data) + n;

    for (int t = 0; t < OTA_MAX_RETRIES; t++) {
        esp_now_send(dst, (const uint8_t *)&d, flen);
        mesh_ota_status_t st;
        if (wait_status(&st, session, OTA_STATUS_TIMEOUT_MS)) {
            if (st.state == OTA_ST_OK || st.state == OTA_ST_NEED) { *off = st.next_offset; return true; }
            if (st.state == OTA_ST_ERR) {
                ESP_LOGE(TAG, "data rejected @%" PRIu32 ": err=%d", cur, (int)st.err);
                return false;
            }
        }
    }
    ESP_LOGE(TAG, "data @%" PRIu32 ": no ack after %d tries", cur, OTA_MAX_RETRIES);
    return false;
}

static bool do_end(const uint8_t *dst, uint32_t session, uint32_t total)
{
    mesh_ota_end_t m = {0};
    fill_hdr(&m.hdr, MSG_OTA_END);
    m.session_id = session;
    m.total_size = total;

    for (int t = 0; t < OTA_MAX_RETRIES; t++) {
        esp_now_send(dst, (const uint8_t *)&m, sizeof(m));
        mesh_ota_status_t st;
        if (wait_status(&st, session, OTA_STATUS_TIMEOUT_MS)) {
            if (st.state == OTA_ST_DONE) return true;
            if (st.state == OTA_ST_ERR)  { ESP_LOGE(TAG, "end err=%d", (int)st.err); return false; }
            if (st.state == OTA_ST_NEED) { ESP_LOGW(TAG, "end: worker wants more from %" PRIu32, st.next_offset); return false; }
        }
        /* No reply: the worker commits then reboots ~300 ms after DONE, so a
         * lost DONE looks like a timeout. Keep retrying; if every try times out
         * we assume it rebooted into the new image (treated as success below). */
    }
    return true;
}

static void push_to(const uint8_t *dst, const esp_partition_t *src, uint32_t total, const char *ver)
{
    uint32_t session = ++s_session_ctr;
    xQueueReset(s_status_q);
    s_pct = 0;

    char ms[18];
    mac_to_str(dst, ms);
    ESP_LOGI(TAG, "push %" PRIu32 " B ver '%s' -> %s (session %" PRIu32 ")", total, ver, ms, session);

    uint32_t off = 0;
    if (!do_begin(dst, session, total, ver, &off)) { s_pct = -1; return; }

    while (off < total) {
        if (!do_data(dst, session, src, total, &off)) { s_pct = -1; return; }
        s_pct = (int)((uint64_t)off * 100 / total);
    }

    bool ok = do_end(dst, session, total);
    ESP_LOGI(TAG, "push %s", ok ? "done (worker rebooting into new image)" : "END not confirmed");
    s_pct = -1;
}

/* ----------------------------- image source ----------------------------- */
#if CONFIG_GATEWAY_OTA_SRC_HTTP
/* ---- leg 1: download to a staging partition over Wi-Fi STA ---- */
#define WIFI_BIT_GOT_IP  BIT0
#define WIFI_BIT_FAIL    BIT1
static EventGroupHandle_t s_wifi_eg;
static bool               s_handlers_ready;

static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)data;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED)
        xEventGroupSetBits(s_wifi_eg, WIFI_BIT_FAIL);
    else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP)
        xEventGroupSetBits(s_wifi_eg, WIFI_BIT_GOT_IP);
}

static bool sta_connect(void)
{
    /* The default STA netif is created in the gateway's wifi_espnow_init()
     * BEFORE esp_wifi_start() (required for the Wi-Fi<->lwIP rx glue + DHCP).
     * Here we only register our event handlers once. */
    if (!s_handlers_ready) {
        s_wifi_eg = xEventGroupCreate();
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt, NULL, NULL);
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_evt, NULL, NULL);
        s_handlers_ready = true;
    }
    wifi_config_t wc = {0};
    snprintf((char *)wc.sta.ssid,     sizeof(wc.sta.ssid),     "%s", CONFIG_GATEWAY_OTA_WIFI_SSID);
    snprintf((char *)wc.sta.password, sizeof(wc.sta.password), "%s", CONFIG_GATEWAY_OTA_WIFI_PASS);
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));

    xEventGroupClearBits(s_wifi_eg, WIFI_BIT_GOT_IP | WIFI_BIT_FAIL);
    ESP_LOGI(TAG, "leg 1: connecting to AP '%s' (ESP-NOW pauses)", CONFIG_GATEWAY_OTA_WIFI_SSID);
    esp_wifi_connect();
    EventBits_t b = xEventGroupWaitBits(s_wifi_eg, WIFI_BIT_GOT_IP | WIFI_BIT_FAIL,
                                        pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
    if (b & WIFI_BIT_GOT_IP) { ESP_LOGI(TAG, "AP connected, got IP"); return true; }
    ESP_LOGE(TAG, "AP connect failed");
    return false;
}

static void sta_restore_channel(void)
{
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(100));
    /* Back to the shared ESP-NOW channel so leg 2 can reach the worker. */
    esp_wifi_set_channel(MESH_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
    ESP_LOGI(TAG, "leg 2: restored channel %d for ESP-NOW", MESH_WIFI_CHANNEL);
}

static bool make_source(const esp_partition_t **out, uint32_t *out_len, char *ver, size_t verlen)
{
    if (!sta_connect()) return false;

    const esp_partition_t *stage = esp_ota_get_next_update_partition(NULL);
    if (!stage) { sta_restore_channel(); return false; }
    ESP_LOGI(TAG, "staging download to '%s' (%" PRIu32 " B slot)", stage->label, stage->size);
    if (esp_partition_erase_range(stage, 0, stage->size) != ESP_OK) { sta_restore_channel(); return false; }

    esp_http_client_config_t cfg = {
        .url               = CONFIG_GATEWAY_OTA_HTTP_URL,
        .crt_bundle_attach = esp_crt_bundle_attach,   /* HTTPS via the cert bundle */
        .timeout_ms        = 10000,
        .keep_alive_enable = true,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    bool ok = false;
    uint32_t off = 0;
    uint8_t *buf = malloc(1024);
    if (cli && buf && esp_http_client_open(cli, 0) == ESP_OK) {
        esp_http_client_fetch_headers(cli);
        for (;;) {
            int r = esp_http_client_read(cli, (char *)buf, 1024);
            if (r < 0) { ESP_LOGE(TAG, "http read error"); break; }
            if (r > 0) {
                if (esp_partition_write(stage, off, buf, r) != ESP_OK) { ESP_LOGE(TAG, "stage write @%" PRIu32, off); break; }
                off += r;
            } else { /* r == 0 */
                if (esp_http_client_is_complete_data_received(cli)) { ok = true; break; }
                if (errno == ECONNRESET || errno == ENOTCONN) { ESP_LOGE(TAG, "conn closed early"); break; }
            }
        }
    } else {
        ESP_LOGE(TAG, "http open/init failed");
    }
    free(buf);
    if (cli) { esp_http_client_close(cli); esp_http_client_cleanup(cli); }

    sta_restore_channel();        /* always go back to the ESP-NOW channel */

    if (!ok || off == 0) { ESP_LOGE(TAG, "leg-1 download failed (%" PRIu32 " B)", off); return false; }
    ESP_LOGI(TAG, "leg-1 download OK: %" PRIu32 " B", off);
    snprintf(ver, verlen, "http");
    *out = stage;
    *out_len = off;
    return true;
}
#else  /* CONFIG_GATEWAY_OTA_SRC_LOOPBACK */
static bool make_source(const esp_partition_t **out, uint32_t *out_len, char *ver, size_t verlen)
{
    const esp_partition_t *run = esp_ota_get_running_partition();
    esp_image_metadata_t meta = {0};
    esp_partition_pos_t pos = { .offset = run->address, .size = run->size };
    if (esp_image_get_metadata(&pos, &meta) != ESP_OK) {
        ESP_LOGE(TAG, "cannot read running image metadata");
        return false;
    }
    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(run, &desc) == ESP_OK)
        snprintf(ver, verlen, "%s", desc.version);
    else
        snprintf(ver, verlen, "loopback");

    ESP_LOGI(TAG, "loopback source '%s': image_len=%" PRIu32 " B", run->label, meta.image_len);
    *out = run;
    *out_len = meta.image_len;
    return true;
}
#endif

/* ----------------------------- trigger / task --------------------------- */
static void configure_button(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << OTA_TRIGGER_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
}

static bool button_pressed(void)
{
    if (gpio_get_level(OTA_TRIGGER_GPIO) != 0) return false;
    vTaskDelay(pdMS_TO_TICKS(30));                 /* debounce */
    return gpio_get_level(OTA_TRIGGER_GPIO) == 0;
}

static void run_once(void)
{
    if (!s_have_target) { ESP_LOGW(TAG, "no worker seen yet - nothing to push"); return; }

    uint8_t dst[6];
    memcpy(dst, (const void *)s_target, 6);

    const esp_partition_t *src = NULL;
    uint32_t len = 0;
    char ver[OTA_FW_VER_LEN] = {0};
    if (make_source(&src, &len, ver, sizeof(ver)) && src && len > 0)
        push_to(dst, src, len, ver);
    else
        s_pct = -1;
}

static void sender_task(void *arg)
{
    (void)arg;
    for (;;) {
        bool go = s_trigger;
        s_trigger = false;
        if (!go && button_pressed()) {
            go = true;
            while (gpio_get_level(OTA_TRIGGER_GPIO) == 0) vTaskDelay(pdMS_TO_TICKS(20)); /* wait release */
        }
        if (go) run_once();
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ----------------------------- public API ------------------------------- */
void ota_sender_init(void)
{
    s_status_q = xQueueCreate(OTA_STATUS_Q_DEPTH, sizeof(mesh_ota_status_t));
    if (!s_status_q) { ESP_LOGE(TAG, "status queue alloc failed"); return; }
    esp_wifi_get_mac(WIFI_IF_STA, s_my_mac);
    configure_button();
    if (xTaskCreate(sender_task, "ota_tx", OTA_TASK_STACK, NULL, OTA_TASK_PRIO, NULL) != pdPASS)
        ESP_LOGE(TAG, "sender task create failed");
    else
        ESP_LOGI(TAG, "OTA sender ready - press BOOT (GPIO%d) to push to the latest worker", OTA_TRIGGER_GPIO);
}

void ota_sender_set_target(const uint8_t *worker_mac)
{
    memcpy((void *)s_target, worker_mac, 6);
    s_have_target = true;
}

void ota_sender_trigger(void)
{
    s_trigger = true;
}

bool ota_sender_rx_status(const uint8_t *src_mac, const uint8_t *data, int len)
{
    (void)src_mac;
    if (len < (int)sizeof(mesh_hdr_t)) return false;
    const mesh_hdr_t *h = (const mesh_hdr_t *)data;
    if (h->magic != MESH_PROTO_MAGIC || h->version != MESH_PROTO_VER) return false;
    if (h->type != MSG_OTA_STATUS) return false;        /* not ours */

    if (s_status_q && len >= (int)sizeof(mesh_ota_status_t)) {
        mesh_ota_status_t st;
        memcpy(&st, data, sizeof(st));
        xQueueSend(s_status_q, &st, 0);
    }
    return true;                                         /* consumed */
}

int ota_sender_progress_pct(void)
{
    return s_pct;
}
