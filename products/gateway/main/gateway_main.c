/*
 * gateway_main.c  -  ESP-NOW initiator / gateway node
 *
 *   MCU     : ESP32-S3
 *   Display : ILI9488 320x480 TFT over SPI (atanisoft/esp_lcd_ili9488)
 *
 * Behaviour:
 *   - receives telemetry from one or more workers over ESP-NOW,
 *   - keeps a small node table and renders it on the TFT,
 *   - on first sight of a worker, registers it as a unicast peer and sends
 *     a MSG_ACK so the worker switches from broadcast to unicast.
 *
 * Build target: ESP-IDF v6.0.0, ESP32-S3, 4 MB flash.
 *
 * NOTE: the ILI9488 over SPI uses 18-bit color (RGB666); the component
 * converts the RGB565 buffer we provide. If colors look swapped, flip
 * .rgb_ele_order between BGR and RGB below. If you see SPI artifacts,
 * lower LCD_PCLK_HZ (try 4 MHz, then increase).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"      /* esp_netif_create_default_wifi_sta() */
#include "esp_mac.h"
#include "esp_now.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_ili9488.h"        /* managed component: atanisoft/esp_lcd_ili9488 */

#include "mesh_proto.h"
#include "font5x7.h"
#include "ota_sender.h"

static const char *TAG = "gateway";

/* ----------------------- display pin map --------------------------- */
#define LCD_PIN_SCK    14
#define LCD_PIN_MOSI   21
#define LCD_PIN_MISO   47
#define LCD_PIN_CS      1
#define LCD_PIN_DC      2
#define LCD_PIN_RST    41
#define LCD_PIN_BL     42
#define LCD_H_RES     320
#define LCD_V_RES     480
#define LCD_HOST      SPI2_HOST
#define LCD_PCLK_HZ   (20 * 1000 * 1000)

/* horizontal-band scratch buffer; every draw stays within 320 x BAND_H */
#define BAND_H        40
#define ILI_BPP       3                 /* RGB666 bytes/pixel after conversion */

/* RGB565 helpers / palette */
#define RGB565(r,g,b) ((uint16_t)((((r)&0xF8)<<8) | (((g)&0xFC)<<3) | ((b)>>3)))
#define C_BLACK   0x0000
#define C_WHITE   0xFFFF
#define C_CYAN    RGB565(0,200,255)
#define C_YELLOW  RGB565(255,210,0)
#define C_GREEN   RGB565(0,220,80)
#define C_RED     RGB565(255,60,60)

static uint16_t s_band[LCD_H_RES * BAND_H];   /* 320*40*2 = 25 600 bytes */
static esp_lcd_panel_handle_t s_lcd;

/* =========================== TFT layer ============================= */
static inline void lcd_blit(int x, int y, int w, int h, const uint16_t *buf)
{
    esp_lcd_panel_draw_bitmap(s_lcd, x, y, x + w, y + h, buf);
}

static void lcd_fill(uint16_t color)
{
    for (int i = 0; i < LCD_H_RES * BAND_H; i++) s_band[i] = color;
    for (int y = 0; y < LCD_V_RES; y += BAND_H) {
        int h = (y + BAND_H <= LCD_V_RES) ? BAND_H : (LCD_V_RES - y);
        lcd_blit(0, y, LCD_H_RES, h, s_band);
    }
}

/* draw one text line (height = FONT_H*scale, must be <= BAND_H) */
static void lcd_text(int x, int y, int scale, uint16_t fg, uint16_t bg, const char *s)
{
    int h = FONT_H * scale; if (h > BAND_H) h = BAND_H;
    int w = 0;
    for (const char *p = s; *p; p++) w += (FONT_W + 1) * scale;
    int maxw = LCD_H_RES - x;
    if (w > maxw) w = maxw;
    if (w <= 0) return;

    for (int i = 0; i < w * h; i++) s_band[i] = bg;   /* w-stride packed */

    int penx = 0;
    for (const char *q = s; *q; q++) {
        const uint8_t *g = font5x7_glyph(*q);
        for (int r = 0; r < FONT_H; r++)
            for (int c = 0; c < FONT_W; c++)
                if ((g[r] >> (FONT_W - 1 - c)) & 1)
                    for (int dy = 0; dy < scale; dy++)
                        for (int dx = 0; dx < scale; dx++) {
                            int px = penx + c * scale + dx;
                            int py = r * scale + dy;
                            if (px < w && py < h) s_band[py * w + px] = fg;
                        }
        penx += (FONT_W + 1) * scale;
        if (penx >= w) break;
    }
    lcd_blit(x, y, w, h, s_band);
}

static void lcd_init(void)
{
    gpio_config_t bl = { .mode = GPIO_MODE_OUTPUT, .pin_bit_mask = 1ULL << LCD_PIN_BL };
    ESP_ERROR_CHECK(gpio_config(&bl));
    gpio_set_level(LCD_PIN_BL, 1);                 /* backlight on */

    spi_bus_config_t buscfg = {
        .sclk_io_num     = LCD_PIN_SCK,
        .mosi_io_num     = LCD_PIN_MOSI,
        .miso_io_num     = LCD_PIN_MISO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_H_RES * BAND_H * ILI_BPP + 64,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &buscfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num       = LCD_PIN_DC,
        .cs_gpio_num       = LCD_PIN_CS,
        .pclk_hz           = LCD_PCLK_HZ,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
        .lcd_cmd_bits      = 8,
        .lcd_param_bits    = 8,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io));

    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_BGR,  /* flip to _RGB if colors look wrong */
        .bits_per_pixel = 18,                         /* ILI9488 over SPI must be 18-bit */
    };
    /* conversion buffer must hold the largest single draw (full band) in RGB666 */
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9488(io, &panel_cfg,
                                              LCD_H_RES * BAND_H * ILI_BPP, &s_lcd));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_lcd));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_lcd));
    esp_lcd_panel_invert_color(s_lcd, false);         /* optional ops: ignore result */
    esp_lcd_panel_disp_on_off(s_lcd, true);
}

/* ========================== node table ============================ */
typedef struct {
    bool     used;
    uint8_t  mac[6];
    int16_t  temp_x10;
    uint16_t rh_x10;
    uint16_t seq;
    uint32_t uptime_s;
    int      rssi;
    int64_t  last_us;
} node_t;

#define MAX_NODES 6
static node_t s_nodes[MAX_NODES];

static node_t *find_or_add(const uint8_t *mac, bool *is_new)
{
    int slot = -1;
    for (int i = 0; i < MAX_NODES; i++) {
        if (s_nodes[i].used && memcmp(s_nodes[i].mac, mac, 6) == 0) { *is_new = false; return &s_nodes[i]; }
        if (!s_nodes[i].used && slot < 0) slot = i;
    }
    if (slot < 0) { *is_new = false; return NULL; }
    s_nodes[slot].used = true;
    memcpy(s_nodes[slot].mac, mac, 6);
    *is_new = true;
    return &s_nodes[slot];
}

static void redraw(void)
{
    lcd_fill(C_BLACK);
    lcd_text(8, 8, 3, C_CYAN, C_BLACK, "ESP-NOW GATEWAY");

    int otap = ota_sender_progress_pct();
    if (otap >= 0) {
        char ol[24];
        snprintf(ol, sizeof(ol), "OTA PUSH %d%%", otap);
        lcd_text(8, 32, 2, C_YELLOW, C_BLACK, ol);
    }

    int64_t now = esp_timer_get_time();
    int y = 50;
    char l[40];
    bool any = false;

    for (int i = 0; i < MAX_NODES; i++) {
        if (!s_nodes[i].used) continue;
        any = true;
        node_t *n = &s_nodes[i];
        char ms[18]; mac_to_str(n->mac, ms);

        snprintf(l, sizeof(l), "NODE %d %s", i, ms);
        lcd_text(8, y, 2, C_YELLOW, C_BLACK, l); y += 22;

        snprintf(l, sizeof(l), "T %d.%dC  RH %u.%u%%",
                 n->temp_x10 / 10, abs(n->temp_x10 % 10), n->rh_x10 / 10, n->rh_x10 % 10);
        lcd_text(8, y, 2, C_WHITE, C_BLACK, l); y += 22;

        int age = (int)((now - n->last_us) / 1000000);
        snprintf(l, sizeof(l), "RSSI %d SEQ %u AGE %ds", n->rssi, (unsigned)n->seq, age);
        lcd_text(8, y, 2, (age < 6) ? C_GREEN : C_RED, C_BLACK, l); y += 28;

        if (y > LCD_V_RES - 30) break;
    }
    if (!any) lcd_text(8, 60, 2, C_WHITE, C_BLACK, "WAITING FOR NODES");
}

/* ========================== ESP-NOW layer ========================= */
typedef struct {
    uint8_t  mac[6];
    int16_t  temp_x10;
    uint16_t rh_x10;
    uint16_t seq;
    uint32_t uptime_s;
    int      rssi;
} rx_evt_t;

static QueueHandle_t s_q;
static uint8_t       s_my_mac[6];
static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static void on_sent(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    (void)tx_info; (void)status;
}

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (len < (int)sizeof(mesh_hdr_t)) return;
    const mesh_hdr_t *h = (const mesh_hdr_t *)data;
    if (h->magic != MESH_PROTO_MAGIC || h->version != MESH_PROTO_VER) return;

    /* MSG_OTA_STATUS replies from a worker drive the OTA sender. */
    if (ota_sender_rx_status(info->src_addr, data, len)) return;

    if (h->type != MSG_TELEMETRY || len < (int)sizeof(mesh_telemetry_t)) return;
    const mesh_telemetry_t *t = (const mesh_telemetry_t *)data;

    rx_evt_t e;
    memcpy(e.mac, info->src_addr, 6);
    e.temp_x10 = t->temp_c_x10;
    e.rh_x10   = t->rh_x10;
    e.seq      = t->hdr.seq;
    e.uptime_s = t->uptime_s;
    e.rssi     = info->rx_ctrl->rssi;
    xQueueSend(s_q, &e, 0);             /* runs in Wi-Fi task ctx: keep it light */
}

static void send_ack(const uint8_t *dst)
{
    if (!esp_now_is_peer_exist(dst)) {
        esp_now_peer_info_t p = {0};
        memcpy(p.peer_addr, dst, 6);
        p.channel = MESH_WIFI_CHANNEL;
        p.ifidx   = WIFI_IF_STA;
        p.encrypt = false;
        if (esp_now_add_peer(&p) != ESP_OK) return;
    }
    mesh_ack_t a = {0};
    a.hdr.magic   = MESH_PROTO_MAGIC;
    a.hdr.version = MESH_PROTO_VER;
    a.hdr.type    = MSG_ACK;
    memcpy(a.hdr.src_mac, s_my_mac, 6);
    memcpy(a.gateway_mac, s_my_mac, 6);
    esp_now_send(dst, (const uint8_t *)&a, sizeof(a));
}

static void wifi_espnow_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

#if CONFIG_GATEWAY_OTA_SRC_HTTP
    /* The leg-1 HTTP download needs an IP-capable STA netif. It MUST be created
     * before esp_wifi_start() so the Wi-Fi<->lwIP receive glue and DHCP client
     * are wired up; creating it lazily after start crashes in esp_netif_receive()
     * (NULL input fn) on the first received frame. ESP-NOW needs no netif, so
     * this is only compiled for the HTTP OTA source. */
    esp_netif_create_default_wifi_sta();
#endif

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_channel(MESH_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE));

    ESP_ERROR_CHECK(esp_now_init());
    ESP_ERROR_CHECK(esp_now_register_send_cb(on_sent));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(on_recv));

    esp_now_peer_info_t bcast = {0};        /* lets the gateway broadcast too if needed */
    memcpy(bcast.peer_addr, BCAST, 6);
    bcast.channel = MESH_WIFI_CHANNEL;
    bcast.ifidx   = WIFI_IF_STA;
    bcast.encrypt = false;
    ESP_ERROR_CHECK(esp_now_add_peer(&bcast));

    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, s_my_mac));
    char macs[18]; mac_to_str(s_my_mac, macs);
    ESP_LOGI(TAG, "gateway STA MAC %s, channel %d", macs, MESH_WIFI_CHANNEL);
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

    s_q = xQueueCreate(16, sizeof(rx_evt_t));

    lcd_init();
    redraw();                            /* shows "WAITING FOR NODES" */
    wifi_espnow_init();
    ota_sender_init();                   /* BOOT button pushes firmware to a worker */

    rx_evt_t e;
    while (1) {
        if (xQueueReceive(s_q, &e, pdMS_TO_TICKS(1000)) == pdTRUE) {
            ota_sender_set_target(e.mac);   /* push targets the latest worker seen */
            bool is_new = false;
            node_t *n = find_or_add(e.mac, &is_new);
            if (n) {
                n->temp_x10 = e.temp_x10;
                n->rh_x10   = e.rh_x10;
                n->seq      = e.seq;
                n->uptime_s = e.uptime_s;
                n->rssi     = e.rssi;
                n->last_us  = esp_timer_get_time();
                if (is_new) send_ack(e.mac);   /* tell this worker to go unicast */
            }
            redraw();
        } else {
            redraw();                          /* refresh AGE counters */
        }
    }
}
