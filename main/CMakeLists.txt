idf_component_register(
    SRCS "PrescriptBeeper.c"
    INCLUDE_DIRS "."
    REQUIRES prescript_drivers esp_wifi esp_event esp_netif nvs_flash
             esp_http_client mbedtls esp_timer
)
