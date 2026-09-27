# Default-off experimental RF command queue (CQ-1). The source is intentionally
# excluded from the firmware until a later stage enables and integrates it.
option(RF_COMMAND_QUEUE_ENABLED "Enable experimental RF private command queue" OFF)
set(RF_CQ_EXTRA_SRCS "")
if(RF_COMMAND_QUEUE_ENABLED)
    list(APPEND RF_CQ_EXTRA_SRCS "rf_command_queue.c" "rf_dispatch_plan.c" "rf_background_schedule.c" "rf_background_query_plan.c" "rf_background_set_plan.c" "rf_background_execution_plan.c" "rf_background_runtime_adapter.c" "rf_background_status_plan.c" "rf_background_command_mapper.c" "rf_background_dispatch_plan.c")
    add_compile_definitions(RF_COMMAND_QUEUE_ENABLED=1)
endif()

# The recovery quiesce component is required by the experimental path. ESP-IDF
# resolves REQUIRES before component-local option values are available, so it is
# listed here unconditionally. Default-off code does not reference it, and the
# linker discards the unused static component from the application.
idf_component_register(SRCS "main.c" ${RF_CQ_EXTRA_SRCS}
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_wifi esp_event esp_netif esp_timer esp_hw_support iot_mqtt_client nvs_flash
                                iot_product_config iot_ssd1306 iot_wifi_sta iot_wifi_connection iot_rf_packet iot_usb_serial_jtag iot_provisioning_wifi_owner ieee802154 esp_coex json
                                iot_provisioning_recovery_quiesce)
