idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES hermes_core driver esp_wifi nvs_flash esp_event esp_netif spiffs esp_timer
)
