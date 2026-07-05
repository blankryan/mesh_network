# Gateway entry point. Shared code (mesh_proto, font5x7, ota_sender) and the
# ILI9488 driver arrive as components via idf_component.yml, so only the app
# itself is compiled here. No role selection: this project IS the gateway.
idf_component_register(SRCS "gateway_main.c"
                       PRIV_REQUIRES esp_lcd driver esp_wifi esp_netif esp_event
                                     nvs_flash esp_timer
                        REQUIRES esp_lcd_ili9488
                        )
