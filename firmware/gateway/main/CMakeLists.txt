# Main component CMakeLists.txt
idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_wifi esp_event esp_netif mqtt nvs_flash
                                iot_product_config ieee802154 esp_coex json)