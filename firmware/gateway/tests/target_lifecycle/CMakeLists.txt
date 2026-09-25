# A minimal ESP32-C6 app for target-level RF command queue lifecycle tests.
# It is intentionally separate from the production gateway project.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(rf_cq_lifecycle_test)
