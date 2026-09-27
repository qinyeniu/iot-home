cmake_minimum_required(VERSION 3.16)
project(iot_rf_packet_host_test C)

enable_testing()

add_executable(test_iot_rf_packet
    test_iot_rf_packet.c
    ../iot_rf_packet.c)
target_include_directories(test_iot_rf_packet PRIVATE ../include)
add_test(NAME iot_rf_packet COMMAND test_iot_rf_packet)