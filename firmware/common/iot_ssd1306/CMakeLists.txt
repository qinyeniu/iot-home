idf_component_register(SRCS "iot_ssd1306.c"
                       INCLUDE_DIRS "include"
                       REQUIRES driver iot_driver_base
                       PRIV_REQUIRES freertos log)
