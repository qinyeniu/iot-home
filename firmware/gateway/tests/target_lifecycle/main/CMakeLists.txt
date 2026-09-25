set(RF_CQ_MAIN_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../main")

idf_component_register(
    SRCS
        "test_rf_cq_lifecycle.c"
        "${RF_CQ_MAIN_DIR}/rf_command_queue.c"
    INCLUDE_DIRS
        "."
        "${RF_CQ_MAIN_DIR}"
)
