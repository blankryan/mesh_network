# ESP-NOW Worker <-> Gateway (ESP-IDF v6.0.0)

Two-node ESP-NOW link demonstrating the data path from the architecture and
robustness docs on real hardware:

- **Worker (responder)** - ESP32 - reads a dummy temperature/humidity sensor,
  shows it on a 0.96" SSD1306 OLED, and sends it over ESP-NOW.
- **Gateway (initiator)** - ESP32-S3 - receives telemetry from one or more
  workers and renders a live node table on an ILI9488 320x480 TFT.

```
ESP32 worker                              ESP32-S3 gateway
+-------------------+   ESP-NOW (ch 1)    +-----------------------+
| dummy sensor      | =================>  | node table            |
| SSD1306 (I2C)     |  telemetry / ACK    | ILI9488 TFT (SPI)     |
+-------------------+ <=================   +-----------------------+
```

The worker starts by **broadcasting**; when the gateway hears a new worker it
registers it as a unicast peer and replies with a small `MSG_ACK`. The worker
then switches to **unicast** (so each send gets a per-hop MAC ACK). This mirrors
the discovery -> unicast pattern in the architecture doc, without hardcoding
any MAC address.

---

## Repository layout

The repo follows the ESP-IDF **multi-product** pattern (DevCon24 build-system
talk): each firmware is its **own ESP-IDF project** under `products/`, and all
code used by more than one product lives in `common_components/`, consumed via
`idf_component.yml` **`path:` dependencies**. There is no role Kconfig, no
`app_main()` collision to work around, and no unused stub source anymore - a
product *is* its role.

```
mesh_network/
├── sdkconfig.defaults               # shared by both products: 4 MB flash, two-OTA table
├── common_components/               # shared code, consumed via `path:` dependencies
│   ├── mesh_proto/                  #   packet format (header-only)
│   ├── font5x7/                     #   5x7 bitmap font (header-only)
│   ├── ota_responder/               #   worker-side ESP-NOW OTA + rollback self-test
│   └── ota_sender/                  #   gateway-side ESP-NOW OTA (+ its Kconfig menu)
└── products/
    ├── worker/                      # standalone project -> build/worker.bin (esp32)
    │   ├── CMakeLists.txt           #   project(worker)
    │   ├── CMakePresets.json        #   pins IDF_TARGET + layers the sdkconfig defaults
    │   ├── sdkconfig.defaults.worker
    │   └── main/
    │       ├── CMakeLists.txt
    │       ├── idf_component.yml    #   path: deps on ../../../common_components/*
    │       └── worker_main.c        #   app_main()
    └── gateway/                     # standalone project -> build/gateway.bin (esp32s3)
        ├── CMakeLists.txt           #   project(gateway)
        ├── CMakePresets.json
        ├── sdkconfig.defaults.gateway
        └── main/
            ├── CMakeLists.txt
            ├── idf_component.yml    #   + atanisoft/esp_lcd_ili9488 (gateway-only now)
            └── gateway_main.c       #   app_main()
```

Notes on the shared components:

- Each product's `main/idf_component.yml` references them with relative
  `path:` entries. Moving a component to its own git repository later is a
  one-line change per consumer: swap `path:` for `git:` (plus the in-repo
  `path:` subfolder and a `version:` git ref).
- Components can depend on each other the same way: `ota_responder` and
  `ota_sender` both declare `mesh_proto: {path: "../mesh_proto"}` in their own
  manifests, so consumers never have to know about that transitive dependency.
- `ota_sender` carries its own `Kconfig.projbuild` ("Gateway OTA sender"), so
  that menu exists only in builds that depend on the component - the old
  `depends on MESH_NODE_ROLE_GATEWAY` guard is gone along with the role choice.
- `atanisoft/esp_lcd_ili9488` is declared only by the gateway product, so the
  worker never downloads or compiles it; the old `rules: if target == esp32s3`
  gate is no longer needed.

---

## Wiring

### Worker - SSD1306 OLED (I2C)

| OLED | ESP32 GPIO | Notes |
|---|---|---|
| SDA | 21 | `I2C_SDA_GPIO` in `worker_main.c` |
| SCL | 22 | `I2C_SCL_GPIO` |
| VCC | 3V3 | |
| GND | GND | |

I2C address defaults to `0x3C` (`OLED_I2C_ADDR`); some modules are `0x3D`. The
worker uses the internal pull-ups; for longer wires add external 4.7k pull-ups.
**The OLED pins were not specified, so 21/22 are assumed - change them to match
your board.**

### Gateway - ILI9488 TFT (SPI)  - pin map as provided

| TFT | ESP32-S3 GPIO |
|---|---|
| SCK | 14 |
| MOSI | 21 |
| MISO | 47 |
| CS | 1 |
| DC | 2 |
| RST | 41 |
| BL (backlight) | 42 |

Resolution 320x480, SPI host `SPI2_HOST`, pixel clock 20 MHz.

---

## Build & flash

ESP-IDF **v6.0.0** must be installed and exported (`. $IDF_PATH/export.sh`, or
`C:\esp\v6.0\esp-idf\export.ps1` on this machine).

Each product is its own project, so either `cd` into it or use `idf.py -C`
from the repo root:

```bash
# Worker (ESP32)
idf.py -C products/worker  build
idf.py -C products/worker  -p PORT flash monitor      # PORT e.g. COM5 or /dev/ttyUSB0

# Gateway (ESP32-S3)
idf.py -C products/gateway build
idf.py -C products/gateway -p PORT flash monitor      # PORT e.g. COM7 or /dev/ttyACM0
```

- Each product ships a `CMakePresets.json` with exactly **one** preset that
  pins the chip (`IDF_TARGET` cache variable - **no `idf.py set-target`
  needed**) and layers the sdkconfig defaults
  (`<repo>/sdkconfig.defaults;<product>/sdkconfig.defaults.<role>`). With a
  single preset per project, `idf.py` picks it automatically; `--preset worker`
  / `--preset gateway` also works explicitly. (An `IDF_TARGET` *environment*
  variable would override the preset; leave it unset.)
- Builds are fully isolated: `products/worker/build` and
  `products/gateway/build`, each with its own `sdkconfig` at the product root.
- Artifacts are now named after the product: `worker.bin` and `gateway.bin`
  (handy for the HTTP OTA source, whose default URL ends in `worker.bin`).
- Other actions work the same way: `idf.py -C products/gateway menuconfig`,
  `idf.py -C products/worker fullclean`, etc.

On the gateway's first build, the component manager downloads
`atanisoft/esp_lcd_ili9488` automatically into
`products/gateway/managed_components/`; the worker build never sees it. The
serial monitor prints each node's STA MAC, which is handy for debugging.

---

## Protocol (`common_components/mesh_proto`)

All packets start with `mesh_hdr_t { magic, version, type, src_mac[6], seq }`.

| Type | Direction | Payload |
|---|---|---|
| `MSG_TELEMETRY` | worker -> gateway | `temp_c_x10` (int16), `rh_x10` (uint16), `uptime_s` (uint32) |
| `MSG_ACK` | gateway -> worker | `gateway_mac[6]` |
| `MSG_OTA_BEGIN` | gateway -> worker | `session_id`, `total_size`, `chunk_size`, `fw_version[24]` |
| `MSG_OTA_DATA` | gateway -> worker | `session_id`, `offset`, `len`, `data[<=192]` |
| `MSG_OTA_END` | gateway -> worker | `session_id`, `total_size` |
| `MSG_OTA_STATUS` | worker -> gateway | `session_id`, `state` (READY/OK/NEED/DONE/ERR), `err`, `next_offset` |

Temperature/humidity are sent as integers scaled by 10 (e.g. `253` = 25.3 C) to
avoid floats on the wire. The telemetry packet is ~24 bytes and the largest OTA
packet (`MSG_OTA_DATA`) is 214 bytes — both under `ESP_NOW_MAX_DATA_LEN` (250 B),
so the format stays ESP-NOW v1.0 compatible if you later add an ESP8266 node.

Both nodes are fixed to **Wi-Fi channel 1** (`MESH_WIFI_CHANNEL`). Because the
protocol is a single shared component, changing it in one place changes it for
every product — no risk of the two firmwares drifting apart.

### OTA over ESP-NOW

Both ends are implemented: the gateway is the **sender/initiator**
([common_components/ota_sender](common_components/ota_sender/ota_sender.c)) and
the worker is the **responder**
([common_components/ota_responder](common_components/ota_responder/ota_responder.c)).
The exchange is **stop-and-wait, in-order**, so the responder uses sequential
`esp_ota_write()` only:

```
gateway --MSG_OTA_BEGIN--> worker   esp_ota_begin() on the inactive slot
gateway --MSG_OTA_DATA-->  worker   esp_ota_write(); reply OK(next_offset)
        <--MSG_OTA_STATUS--         ...or NEED(next_offset) to resume on loss
gateway --MSG_OTA_END-->   worker   esp_ota_end() + esp_ota_set_boot_partition()
        <--MSG_OTA_STATUS-- worker   DONE, then esp_restart()
```

**Sender (gateway).** Press the **BOOT button (GPIO0)** to push firmware to the
most recently seen worker; the TFT shows `OTA PUSH nn%`. The image source is a
Kconfig choice (`idf.py -C products/gateway menuconfig` → *Gateway OTA sender*,
a menu provided by the `ota_sender` component):

- **Loopback** (default): streams the gateway's own running app (true length via
  `esp_image_get_metadata`). No AP/server needed. **Chip caveat:** the gateway is
  `esp32s3` and the worker is `esp32`, so the worker's `esp_ota_end()` will
  **correctly reject** the wrong-chip image (`ESP_ERR_OTA_VALIDATE_FAILED`) —
  loopback exercises the whole transport + the validation safety-net, but to see a
  *successful* update + rollback you need a **same-chip** image (use the HTTP
  source with a real worker `.bin` — conveniently now named `worker.bin` — or two
  same-chip boards).
- **HTTP/HTTPS**: "leg 1" downloads a worker image over Wi-Fi STA into a flash
  staging slot, then "leg 2" pushes it. The legs are **sequential** because
  joining an AP moves the STA off the ESP-NOW channel; set the URL/SSID/password
  in menuconfig. Uses the certificate bundle for HTTPS.

**Responder (worker).** On the **first boot** of the new image (state
`PENDING_VERIFY`, since `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y` on the worker)
it runs a self-test and calls `esp_ota_mark_app_valid_cancel_rollback()` on
success or `esp_ota_mark_app_invalid_rollback_and_reboot()` on failure; a crash /
hang / power-loss before that confirm is auto-rolled-back by the bootloader. The
OLED shows `OTA nn%` during a transfer. To test the rollback path, set
`OTA_FORCE_SELFTEST_FAIL 1` in
[common_components/ota_responder/ota_responder.c](common_components/ota_responder/ota_responder.c)
and push an image.

---

## What you should see

- **Worker OLED:** `WORKER NODE`, large temperature and humidity, and a status
  line `LINK BCAST` that becomes `LINK UNICAST` once the gateway is found.
- **Gateway TFT:** `ESP-NOW GATEWAY` title, then one block per worker with MAC,
  `T / RH`, and `RSSI / SEQ / AGE`. The AGE line turns red if a node hasn't been
  heard from in >5 s. Pressing **BOOT** starts an OTA push and shows `OTA PUSH nn%`.

---

## Caveats / things to tune on hardware

1. **Not compiled against hardware here.** The code targets the documented
   ESP-IDF v6.0.0 APIs (verified against the docs and the ILI9488 component
   source), but you should expect to tweak pins/clocks on your boards.
2. **Font glyphs** are a minimal hand-authored 5x7 set - the one part worth
   eyeballing on screen. For a richer UI, replace the framebuffer + font with
   **LVGL** (both `esp_lcd` panels integrate with `esp_lvgl_port`).
3. **TFT colors swapped?** Flip `.rgb_ele_order` between `LCD_RGB_ELEMENT_ORDER_BGR`
   and `..._RGB` in `lcd_init()`.
4. **TFT artifacts / blank screen?** Lower `LCD_PCLK_HZ` (try 4 MHz, then raise),
   and double-check RST/DC/CS wiring. ILI9488 over SPI *must* run 18-bit color -
   that is why `bits_per_pixel = 18` and the conversion-buffer argument are set.
5. **Same channel, no router.** This setup assumes no Wi-Fi uplink. If the
   gateway later joins an AP, ESP-NOW is locked to that AP's channel - set both
   nodes accordingly.
6. **Security not wired in.** This is the plain data path. Add native PMK/LMK
   encryption (or the `espressif/esp-now` component) for security. The two-bank +
   rollback OTA flow from the robustness doc is implemented on both ends; see
   "OTA over ESP-NOW" above.
7. **OTA speed.** Stop-and-wait over ESP-NOW is reliable but not fast: a ~1 MB
   image is thousands of round-trips. Fine for a demo; for production consider a
   windowed/batched ACK scheme.

---

## Next steps I can help with

- Replace the dummy sensor with a real driver (e.g. SHT3x/SHT4x on the same I2C
  bus) - as its own `common_components/` driver, ready for the next product.
- Publish `common_components/` to a separate git repository and switch the
  `path:` references to `git:`.
- Target a *specific* worker for OTA (e.g. pick from the node table) instead of
  "the latest seen", and add a windowed ACK scheme to speed up large pushes.
- Port both displays to LVGL for nicer rendering.
- Add ESP-NOW encryption (PMK/LMK) and a heartbeat/offline timeout on the gateway.
