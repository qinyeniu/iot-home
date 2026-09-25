# Default-off experimental RF command queue (CQ-1). The source is intentionally
# excluded from the firmware until a later stage enables and integrates it.
option(RF_COMMAND_QUEUE_ENABLED "Enable experimental RF private command queue" OFF)
set(RF_CQ_EXTRA_SRCS "")
if(RF_COMMAND_QUEUE_ENABLED)
    list(APPEND RF_CQ_EXTRA_SRCS "rf_command_queue.c" "rf_dispatch_plan.c")
    add_compile_definitions(RF_COMMAND_QUEUE_ENABLED=1)
endif()

idf_component_register(SRCS "main.c" ${RF_CQ_EXTRA_SRCS}
                       INCLUDE_DIRS "."
                       REQUIRES driver esp_wifi esp_event esp_netif mqtt nvs_flash
                                iot_product_config ieee802154 esp_coex json)
