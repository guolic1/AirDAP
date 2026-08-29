cmake_minimum_required(VERSION 3.16)

project(airdap_wired_hil_helper_test NONE)

enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_test(
    NAME wired_hil_helper
    COMMAND
        ${Python3_EXECUTABLE}
        -m unittest
        discover
        -s ${CMAKE_CURRENT_SOURCE_DIR}
        -p test_wired_smoke.py
)
