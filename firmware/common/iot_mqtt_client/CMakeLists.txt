idf_component_register(
    SRCS "iot_mqtt_client.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES mqtt esp_event log
)