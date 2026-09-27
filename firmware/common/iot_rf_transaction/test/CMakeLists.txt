cmake_minimum_required(VERSION 3.16)
project(iot_rf_transaction_host_test C)

enable_testing()

add_executable(test_iot_rf_transaction
    test_iot_rf_transaction.c
    ../iot_rf_transaction.c)
target_include_directories(test_iot_rf_transaction PRIVATE ../include)
add_test(NAME iot_rf_transaction COMMAND test_iot_rf_transaction)
