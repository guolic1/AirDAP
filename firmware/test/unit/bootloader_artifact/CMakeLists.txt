cmake_minimum_required(VERSION 3.22)

project(airdap_bootloader_artifact_tests NONE)

enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_test(
    NAME bootloader_artifact_contract
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_verify_bootloader.py
)
