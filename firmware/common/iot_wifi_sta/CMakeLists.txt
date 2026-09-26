idf_component_register(
    SRCS "iot_wifi_sta.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi
    PRIV_REQUIRES esp_event esp_netif log
)
