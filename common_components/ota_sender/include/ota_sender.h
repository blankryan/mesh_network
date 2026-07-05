/*
 * ota_sender.h - initiator/sender-side ESP-NOW OTA for the gateway node.
 *
 * Drives the stop-and-wait MSG_OTA_* protocol that the worker (responder)
 * implements in ota_responder.c. Two image sources (Kconfig-selected):
 *
 *   - LOOPBACK: stream the gateway's OWN running app image to a worker. Needs no
 *     AP/server, so it exercises the responder (incl. rollback) end-to-end. The
 *     worker ends up running a copy of the gateway's firmware - fine for a test.
 *   - HTTP: "leg 1" download a worker image over Wi-Fi STA into a flash staging
 *     slot, then "leg 2" push it over ESP-NOW. Legs are SEQUENTIAL because
 *     joining an AP moves the STA off the ESP-NOW channel.
 *
 * A push is triggered by the BOOT button (GPIO0) and targets the most recently
 * seen worker (ota_sender_set_target()), or call ota_sender_trigger() directly.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>

/* Create the sender task + BOOT-button trigger. Call once after ESP-NOW is up. */
void ota_sender_init(void);

/* Remember which worker a triggered push should target (latest telemetry source). */
void ota_sender_set_target(const uint8_t *worker_mac);

/* Start a push to the current target now (same as a BOOT-button press). */
void ota_sender_trigger(void);

/*
 * Feed a received ESP-NOW frame to the sender. Safe from the recv callback
 * (Wi-Fi task context): copies MSG_OTA_STATUS frames to the sender's queue.
 * Returns true if the frame was an OTA status (consumed), false otherwise.
 */
bool ota_sender_rx_status(const uint8_t *src_mac, const uint8_t *data, int len);

/* Push progress for the UI: -1 when idle, else 0..100. */
int ota_sender_progress_pct(void);
