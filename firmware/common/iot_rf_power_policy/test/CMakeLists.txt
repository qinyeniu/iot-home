cmake_minimum_required(VERSION 3.16)
project(iot_rf_power_policy_host_test C)

enable_testing()

add_executable(test_iot_rf_power_policy
    test_iot_rf_power_policy.c
    ../iot_rf_power_policy.c)
target_include_directories(test_iot_rf_power_policy PRIVATE ../include)
add_test(NAME iot_rf_power_policy COMMAND test_iot_rf_power_policy)
