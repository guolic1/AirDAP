cmake_minimum_required(VERSION 3.22)
project(airdap_dap_protocol_test C)

enable_testing()

add_executable(
    test_dap_protocol
    test_dap_protocol.c
    ../../../components/dap_protocol/dap_protocol.c
)

target_include_directories(
    test_dap_protocol
    PRIVATE ../../../components/dap_protocol/include
)

target_compile_options(
    test_dap_protocol
    PRIVATE -Wall -Wextra -Werror -pedantic
)

add_test(NAME dap_protocol COMMAND test_dap_protocol)
