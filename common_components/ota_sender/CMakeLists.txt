# Initiator/sender-side ESP-NOW OTA (used by the gateway product).
# Brings its own Kconfig.projbuild ("Gateway OTA sender" menu), so the menu
# only exists in builds that actually depend on this component.
# mesh_proto arrives via the `path:` dependency in idf_component.yml.
idf_component_register(SRCS "ota_sender.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES app_update esp_partition bootloader_support
                                     esp_wifi esp_timer driver
                                     esp_netif esp_event esp_http_client esp-tls mbedtls)
