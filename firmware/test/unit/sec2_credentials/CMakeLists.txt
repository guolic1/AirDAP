cmake_minimum_required(VERSION 3.22)

project(airdap_sec2_credentials_tests NONE)

enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_test(
    NAME sec2_credentials_tool
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_sec2_credentials.py
)
