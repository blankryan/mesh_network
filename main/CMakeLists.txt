# Multi-config: exactly ONE role source is compiled, because worker_main.c and
# gateway_main.c each define their own app_main(). The role comes from Kconfig
# (CONFIG_MESH_NODE_ROLE_*), which the sdkconfig.defaults.worker /
# sdkconfig.defaults.gateway files set per build configuration. See the README
# "Multiple build configurations" section and main/Kconfig.projbuild.
#
# ota_responder.c (responder-side ESP-NOW OTA) is compiled into both roles but
# only used by the worker; it defines no app_main so there is no link conflict.
#
# main.c is intentionally NOT built (it is an empty stub with a second
# app_main()); do not add it back alongside a role source or the link will fail
# with a duplicate app_main.
if(CONFIG_MESH_NODE_ROLE_GATEWAY)
    set(role_src "gateway_main.c")
else()
    set(role_src "worker_main.c")   # default role (matches MESH_NODE_ROLE_WORKER)
endif()

idf_component_register(SRCS "ota_sender.c" "ota_responder.c" "${role_src}"
                    INCLUDE_DIRS "."
                    REQUIRES esp_lcd driver esp_wifi esp_netif esp_event nvs_flash esp_timer
                             app_update esp_partition bootloader_support esp_http_client esp-tls
)
