cmake_minimum_required(VERSION 3.16)

project(airdap_dap_probe_test NONE)

enable_testing()
find_package(Python3 3.13 REQUIRED COMPONENTS Interpreter)

add_test(
    NAME airdap_dap_probe_host_tool
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_airdap_dap_probe.py
)
