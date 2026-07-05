# Responder-side ESP-NOW OTA (used by the worker product).
# mesh_proto arrives via the `path:` dependency in idf_component.yml.
idf_component_register(SRCS "ota_responder.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES app_update esp_partition esp_wifi)
