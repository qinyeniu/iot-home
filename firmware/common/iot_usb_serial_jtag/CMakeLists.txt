idf_component_register(SRCS "iot_usb_serial_jtag.c"
                       INCLUDE_DIRS "include"
                       REQUIRES iot_driver_base esp_common
                       PRIV_REQUIRES driver freertos)
