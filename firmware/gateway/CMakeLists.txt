# ESP-IDF CMakeLists.txt for IoT-Home Gateway
cmake_minimum_required(VERSION 3.16)

# Shared productization components.
list(APPEND EXTRA_COMPONENT_DIRS "../common/iot_product_config"
                             "../common/iot_provisioning_session"
                             "../common/iot_provisioning_command"
                             "../common/iot_provisioning_wifi_owner"
                             "../common/iot_provisioning_wifi_event"
                             "../common/iot_provisioning_wifi_phase"
                             "../common/iot_provisioning_worker"
                             "../common/iot_provisioning_recovery_quiesce"
                             "../common/iot_provisioning_http")

# Include the ESP-IDF CMake functions
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Define the project
project(iot-home-gateway)
