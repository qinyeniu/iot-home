idf_component_register(SRCS "iot_product_config.c"
                       "iot_credentials_validation.c"
                       "iot_mqtt_uri_validation.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES nvs_flash freertos)
