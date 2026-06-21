/*
 * ota_responder.h - responder-side ESP-NOW OTA for the worker node.
 *
 * Implements "leg 2" of esp_now_ota_robustness.md: receive a firmware image in
 * chunks over ESP-NOW and commit it with the standard esp_ota_* API, plus the
 * boot-time PENDING_VERIFY self-test that arms automatic rollback.
 *
 * Requires CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y and a two-OTA partition
 * table (set in sdkconfig.defaults / sdkconfig.defaults.worker).
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/*
 * Boot-time rollback gate. Call ONCE, early in app_main() AFTER the subsystems
 * the self-test depends on are up (Wi-Fi + ESP-NOW). If the running image is in
 * ESP_OTA_IMG_PENDING_VERIFY (i.e. this is the first boot after an OTA), it runs
 * the self-test and either marks the app valid (cancelling rollback) or marks it
 * invalid and reboots into the previous image. No-op on a normally-flashed app.
 */
void ota_responder_boot_check(void);

/* Create the OTA work queue and task. Call once, after ota_responder_boot_check(). */
void ota_responder_start(void);

/*
 * Feed a received ESP-NOW frame to the OTA receiver. Safe to call from the
 * ESP-NOW recv callback (Wi-Fi task context): it only validates the header and
 * copies OTA frames into the queue. Returns true if the frame was an OTA message
 * (and was consumed), false otherwise so the caller can handle it.
 */
bool ota_responder_rx(const uint8_t *src_mac, const uint8_t *data, int len);

/* OTA progress for the UI: -1 when idle, else 0..100 (100 = unknown total size). */
int ota_responder_progress_pct(void);
