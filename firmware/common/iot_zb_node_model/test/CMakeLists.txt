cmake_minimum_required(VERSION 3.16)
project(iot_zb_node_model_host_test C)

enable_testing()

add_executable(test_iot_zb_node_model
    test_iot_zb_node_model.c
    ../iot_zb_node_model.c)
target_include_directories(test_iot_zb_node_model PRIVATE ../include)
target_link_libraries(test_iot_zb_node_model m)
add_test(NAME iot_zb_node_model COMMAND test_iot_zb_node_model)