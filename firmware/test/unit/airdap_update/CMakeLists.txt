cmake_minimum_required(VERSION 3.16)

project(airdap_update_host_test NONE)

enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_test(
    NAME airdap_update_host
    COMMAND
        ${Python3_EXECUTABLE}
        -m unittest
        discover
        -s ${CMAKE_CURRENT_SOURCE_DIR}
        -p test_airdap_update.py
)
