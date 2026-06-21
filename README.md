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

This is a **single ESP-IDF project** that produces two firmwares (worker and
gateway) via **ESP-IDF v6.0 CMake Presets** (the *Multiple Build Configurations*
feature) — not two separate projects.

```
mesh_network/
├── CMakeLists.txt              # project(mesh_network)
├── CMakePresets.json           # "worker" and "gateway" build presets
├── sdkconfig.defaults          # common: 4 MB flash, two-OTA partition table
├── sdkconfig.defaults.worker   # role=worker  override
├── sdkconfig.defaults.gateway  # role=gateway override
└── main/
    ├── CMakeLists.txt          # compiles ONE role source (chosen by Kconfig)
    ├── Kconfig.projbuild       # MESH_NODE_ROLE choice (worker | gateway)
    ├── idf_component.yml       # esp_lcd_ili9488, gated to esp32s3 (gateway)
    ├── mesh_proto.h            # shared packet format
    ├── font5x7.h               # shared 5x7 font
    ├── worker_main.c           # app_main() for the worker
    ├── gateway_main.c          # app_main() for the gateway
    └── main.c                  # unused stub (NOT compiled — see main/CMakeLists.txt)
```

`worker_main.c` and `gateway_main.c` each define their own `app_main()`, so
**exactly one is compiled per build configuration**. The role is a Kconfig choice
(`CONFIG_MESH_NODE_ROLE_*`) set by the `sdkconfig.defaults.worker` /
`sdkconfig.defaults.gateway` files; `main/CMakeLists.txt` selects the matching
source. Each preset sets its chip via the `IDF_TARGET` cache variable and builds
into its own directory (`build/worker`, `build/gateway`) with its own sdkconfig.

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

## Build & flash (CMake Presets)

ESP-IDF **v6.0.0** must be installed and exported (`. $IDF_PATH/export.sh`, or
`C:\esp\v6.0\esp-idf\export.ps1` on this machine). CMake Presets require
ESP-IDF ≥ v6.0.

Select a configuration with `idf.py --preset <name>`; the rest of the command
line is ordinary `idf.py` actions/arguments:

```bash
# Worker (ESP32)
idf.py --preset worker build
idf.py --preset worker -p PORT flash monitor      # PORT e.g. COM5 or /dev/ttyUSB0

# Gateway (ESP32-S3)
idf.py --preset gateway build
idf.py --preset gateway -p PORT flash monitor     # PORT e.g. COM7 or /dev/ttyACM0
```

- Each preset builds into its own directory (`build/worker`, `build/gateway`)
  with its own `sdkconfig`, so the two configs never clobber each other.
- The chip is selected by the preset's `IDF_TARGET` cache variable — **no
  `idf.py set-target` needed**. (An `IDF_TARGET` *environment* variable would
  override it; leave it unset.)
- To avoid typing `--preset` every time, set the `IDF_PRESET` environment
  variable instead. With no preset and no `IDF_PRESET`, `idf.py` uses the first
  preset (`worker`).
- Other actions work the same way: `idf.py --preset gateway menuconfig`,
  `idf.py --preset worker fullclean`, etc.

On the gateway's first build, the component manager downloads
`atanisoft/esp_lcd_ili9488` automatically; the worker build skips it (gated to
the `esp32s3` target in `idf_component.yml`). The serial monitor prints each
node's STA MAC, which is handy for debugging.

---

## Protocol (`mesh_proto.h`)

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

Both nodes are fixed to **Wi-Fi channel 1** (`MESH_WIFI_CHANNEL`). Change it in
one place and both must match.

### OTA over ESP-NOW

Both ends are implemented: the gateway is the **sender/initiator**
([main/ota_sender.c](main/ota_sender.c)) and the worker is the **responder**
([main/ota_responder.c](main/ota_responder.c)). The exchange is **stop-and-wait, in-order**, so the
responder uses sequential `esp_ota_write()` only:

```
gateway --MSG_OTA_BEGIN--> worker   esp_ota_begin() on the inactive slot
gateway --MSG_OTA_DATA-->  worker   esp_ota_write(); reply OK(next_offset)
        <--MSG_OTA_STATUS--         ...or NEED(next_offset) to resume on loss
gateway --MSG_OTA_END-->   worker   esp_ota_end() + esp_ota_set_boot_partition()
        <--MSG_OTA_STATUS-- worker   DONE, then esp_restart()
```

**Sender (gateway).** Press the **BOOT button (GPIO0)** to push firmware to the
most recently seen worker; the TFT shows `OTA PUSH nn%`. The image source is a
Kconfig choice (`idf.py --preset gateway menuconfig` → *Gateway OTA sender*):

- **Loopback** (default): streams the gateway's own running app (true length via
  `esp_image_get_metadata`). No AP/server needed. **Chip caveat:** the gateway is
  `esp32s3` and the worker is `esp32`, so the worker's `esp_ota_end()` will
  **correctly reject** the wrong-chip image (`ESP_ERR_OTA_VALIDATE_FAILED`) —
  loopback exercises the whole transport + the validation safety-net, but to see a
  *successful* update + rollback you need a **same-chip** image (use the HTTP
  source with a real worker `.bin`, or two same-chip boards).
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
`OTA_FORCE_SELFTEST_FAIL 1` in [main/ota_responder.c](main/ota_responder.c) and push an image.

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

- Replace the dummy sensor with a real driver (e.g. SHT3x/SHT4x on the same I2C bus).
- Target a *specific* worker for OTA (e.g. pick from the node table) instead of
  "the latest seen", and add a windowed ACK scheme to speed up large pushes.
- Port both displays to LVGL for nicer rendering.
- Add ESP-NOW encryption (PMK/LMK) and a heartbeat/offline timeout on the gateway.
