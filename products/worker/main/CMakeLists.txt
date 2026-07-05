# Worker entry point. All shared code (mesh_proto, font5x7, ota_responder)
# arrives as components via idf_component.yml `path:` dependencies, so only the
# app itself is compiled here. No role selection: this project IS the worker.
idf_component_register(SRCS "worker_main.c"
                       PRIV_REQUIRES esp_lcd driver esp_wifi esp_netif esp_event
                                     nvs_flash esp_timer)
