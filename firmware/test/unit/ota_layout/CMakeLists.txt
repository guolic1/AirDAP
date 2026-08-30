cmake_minimum_required(VERSION 3.22)

project(airdap_ota_layout_tests NONE)

enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_test(
    NAME ota_layout_contract
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_verify_ota_layout.py
)
