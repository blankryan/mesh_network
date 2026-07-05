/*
 * worker_main.c  -  ESP-NOW worker / responder node
 *
 *   MCU     : ESP32
 *   Display : 0.96" SSD1306 OLED, 128x64, I2C  (core esp_lcd driver)
 *   Sensor  : dummy temperature + humidity (random walk)
 *
 * Behaviour:
 *   - reads the (dummy) sensor every TELEMETRY_PERIOD_MS,
 *   - shows it on the OLED,
 *   - broadcasts it over ESP-NOW until the gateway answers with an ACK,
 *     then switches to unicast (so transmits get a per-hop MAC ACK).
 *
 * Build target: ESP-IDF v6.0.0, ESP32, 4 MB flash.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"   /* esp_lcd_new_panel_ssd1306 + config */
#include "esp_lcd_panel_ops.h"

#include "mesh_proto.h"
#include "font5x7.h"
#include "ota_responder.h"

static const char *TAG = "worker";

/* ----------------------- user configuration ------------------------ */
#define I2C_SDA_GPIO        21          /* adjust to your wiring */
#define I2C_SCL_GPIO        22
#define OLED_I2C_ADDR       0x3C        /* 0x3C typical, some are 0x3D */
#define OLED_W              128
#define OLED_H              64
#define OLED_I2C_HZ         (400 * 1000)

#define TELEMETRY_PERIOD_MS 2000

/* -------------------------------------------------------------------- */
static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* gateway discovery state (written from the ESP-NOW recv callback) */
static volatile bool s_have_gw       = false;
static volatile bool s_gw_peer_added = false;
static uint8_t       s_gw_mac[6];

/* dummy sensor state */
static int16_t  s_temp_x10 = 250;       /* 25.0 C */
static uint16_t s_rh_x10   = 600;       /* 60.0 % */
static uint16_t s_seq      = 0;
static uint8_t  s_my_mac[6];

/* ============================ OLED layer ============================ */
static uint8_t s_fb[OLED_W * OLED_H / 8];   /* page-packed: 8 vertical px per byte */
static esp_lcd_panel_handle_t s_oled;

static inline void ssd_px(int x, int y, bool on)
{
    if (x < 0 || x >= OLED_W || y < 0 || y >= OLED_H) return;
    uint32_t idx = ((uint32_t)(y >> 3) * OLED_W) + x;
    uint8_t  bit = (uint8_t)(1u << (y & 7));
    if (on) s_fb[idx] |= bit; else s_fb[idx] &= (uint8_t)~bit;
}

static void ssd_char(int x, int y, char c, int scale)
{
    const uint8_t *g = font5x7_glyph(c);
    for (int r = 0; r < FONT_H; r++)
        for (int col = 0; col < FONT_W; col++)
            if ((g[r] >> (FONT_W - 1 - col)) & 1)
                for (int dy = 0; dy < scale; dy++)
                    for (int dx = 0; dx < scale; dx++)
                        ssd_px(x + col * scale + dx, y + r * scale + dy, true);
}

static void ssd_str(int x, int y, const char *s, int scale)
{
    while (*s) { ssd_char(x, y, *s++, scale); x += (FONT_W + 1) * scale; }
}

static inline void ssd_clear(void) { memset(s_fb, 0, sizeof(s_fb)); }
static inline void ssd_flush(void) { esp_lcd_panel_draw_bitmap(s_oled, 0, 0, OLED_W, OLED_H, s_fb); }

static void oled_init(void)
{
    i2c_master_bus_handle_t bus = NULL;
    i2c_master_bus_config_t bus_cfg = {
        .clk_source                 = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt          = 7,
        .i2c_port                   = I2C_NUM_0,
        .sda_io_num                 = I2C_SDA_GPIO,
        .scl_io_num                 = I2C_SCL_GPIO,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_i2c_config_t io_cfg = {
        .dev_addr            = OLED_I2C_ADDR,
        .scl_speed_hz        = OLED_I2C_HZ,
        .control_phase_bytes = 1,   /* per SSD1306 datasheet */
        .dc_bit_offset       = 6,   /* per SSD1306 datasheet */
        .lcd_cmd_bits        = 8,
        .lcd_param_bits      = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .bits_per_pixel = 1,
        .reset_gpio_num = GPIO_NUM_NC,      /* most OLED modules have no RST pin */
    };
    esp_lcd_panel_ssd1306_config_t ssd_cfg = { .height = OLED_H };
    panel_cfg.vendor_config = &ssd_cfg;

    ESP_ERROR_CHECK(esp_lcd_new_panel_ssd1306(io, &panel_cfg, &s_oled));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_oled));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_oled));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_oled, true));
    ssd_clear();
    ssd_flush();
}

static void oled_show(void)
{
    char line[24];
    ssd_clear();
    ssd_str(0, 0, "WORKER NODE", 1);

    snprintf(line, sizeof(line), "T %d.%dC", s_temp_x10 / 10, abs(s_temp_x10 % 10));
    ssd_str(0, 16, line, 2);

    snprintf(line, sizeof(line), "H %u.%u%%", s_rh_x10 / 10, s_rh_x10 % 10);
    ssd_str(0, 40, line, 2);

    int otap = ota_responder_progress_pct();
    if (otap >= 0) {
        snprintf(line, sizeof(line), "OTA %d%%", otap);
        ssd_str(0, 57, line, 1);
    } else {
        ssd_str(0, 57, s_have_gw ? "LINK UNICAST" : "LINK BCAST", 1);
    }
    ssd_flush();
}

/* ========================== ESP-NOW layer ========================== */
static void on_sent(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    (void)tx_info;   /* status is a per-hop MAC ACK only, not end-to-end */
    if (status != ESP_NOW_SEND_SUCCESS)
        ESP_LOGW(TAG, "tx not acked");
}

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len < (int)sizeof(mesh_hdr_t)) return;
    const mesh_hdr_t *h = (const mesh_hdr_t *)data;
    if (h->magic != MESH_PROTO_MAGIC || h->version != MESH_PROTO_VER) return;

    /* OTA frames (gateway -> worker) are copied to the OTA responder's queue and
     * handled in its own task; the recv callback must stay light. */
    if (ota_responder_rx(info->src_addr, data, len)) return;

    if (h->type == MSG_ACK && len >= (int)sizeof(mesh_ack_t)) {
        const mesh_ack_t *a = (const mesh_ack_t *)data;
        memcpy(s_gw_mac, a->gateway_mac, 6);
        s_have_gw = true;             /* main loop adds the unicast peer */
        (void)info;
    }
}

static void wifi_espnow_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(MESH_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_sent));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));

    esp_now_peer_info_t bcast = {0};
    memcpy(bcast.peer_addr, BCAST, 6);
    bcast.channel = MESH_WIFI_CHANNEL;
    bcast.ifidx   = WIFI_IF_STA;
    bcast.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&bcast));

    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, s_my_mac));
    char macs[18];
    mac_to_str(s_my_mac, macs);
    ESP_LOGI(TAG, "worker STA MAC %s, channel %d", macs, MESH_WIFI_CHANNEL);
}

/* ========================= dummy sensor =========================== */
static void read_dummy_sensor(void)
{
    int32_t dt = (int32_t)(esp_random() % 7) - 3;     /* -0.3 .. +0.3 C */
    s_temp_x10 = (int16_t)(s_temp_x10 + dt);
    if (s_temp_x10 < 150) s_temp_x10 = 150;
    if (s_temp_x10 > 350) s_temp_x10 = 350;

    int32_t dh = (int32_t)(esp_random() % 7) - 3;     /* -0.3 .. +0.3 % */
    int32_t rh = (int32_t)s_rh_x10 + dh;
    if (rh < 300) rh = 300;
    if (rh > 900) rh = 900;
    s_rh_x10 = (uint16_t)rh;
}

/* ============================== main ============================== */
void app_main(void)
{
    esp_err_t r = nvs_flash_init();
    if (r == ESP_ERR_NVS_NO_FREE_PAGES || r == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        r = nvs_flash_init();
    }
    ESP_ERROR_CHECK(r);

    oled_init();
    wifi_espnow_init();

    /* Rollback gate + OTA receiver. Run AFTER Wi-Fi/ESP-NOW are up so the
     * PENDING_VERIFY self-test can exercise them. See ota_responder.c. */
    ota_responder_boot_check();
    ota_responder_start();

    while (1) {
        read_dummy_sensor();
        s_seq++;

        /* once the gateway is known, add it as a unicast peer (one time) */
        if (s_have_gw && !s_gw_peer_added) {
            esp_now_peer_info_t gp = {0};
            memcpy(gp.peer_addr, s_gw_mac, 6);
            gp.channel = MESH_WIFI_CHANNEL;
            gp.ifidx   = WIFI_IF_STA;
            gp.encrypt = false;
            if (esp_now_add_peer(&gp) == ESP_OK) {
                s_gw_peer_added = true;
                char macs[18]; mac_to_str(s_gw_mac, macs);
                ESP_LOGI(TAG, "gateway found %s -> unicast", macs);
            }
        }

        mesh_telemetry_t t = {0};
        t.hdr.magic   = MESH_PROTO_MAGIC;
        t.hdr.version = MESH_PROTO_VER;
        t.hdr.type    = MSG_TELEMETRY;
        memcpy(t.hdr.src_mac, s_my_mac, 6);
        t.hdr.seq     = s_seq;
        t.temp_c_x10  = s_temp_x10;
        t.rh_x10      = s_rh_x10;
        t.uptime_s    = (uint32_t)(esp_timer_get_time() / 1000000ULL);

        const uint8_t *dst = (s_have_gw && s_gw_peer_added) ? s_gw_mac : BCAST;
        esp_err_t e = esp_now_send(dst, (const uint8_t *)&t, sizeof(t));
        if (e != ESP_OK) ESP_LOGW(TAG, "esp_now_send: %s", esp_err_to_name(e));

        oled_show();
        vTaskDelay(pdMS_TO_TICKS(TELEMETRY_PERIOD_MS));
    }
}
