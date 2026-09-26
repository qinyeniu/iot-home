idf_component_register(
    SRCS "iot_wifi_connection.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_wifi esp_event esp_netif freertos log
)
