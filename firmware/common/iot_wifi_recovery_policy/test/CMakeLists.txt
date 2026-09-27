cmake_minimum_required(VERSION 3.16)
project(iot_wifi_recovery_policy_host_test C)

enable_testing()

add_executable(test_iot_wifi_recovery_policy
    test_iot_wifi_recovery_policy.c
    ../iot_wifi_recovery_policy.c)
target_include_directories(test_iot_wifi_recovery_policy PRIVATE ../include)
add_test(NAME iot_wifi_recovery_policy COMMAND test_iot_wifi_recovery_policy)
