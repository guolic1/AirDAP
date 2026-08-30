cmake_minimum_required(VERSION 3.16)

project(airdap_dap_stream_test C)

enable_testing()

add_executable(
    dap_stream_test
    test_dap_stream.c
    ../../../components/usb_device/dap_stream.c
    ../../../components/dap_protocol/dap_protocol.c
)
target_include_directories(
    dap_stream_test
    PRIVATE
        ../../../components/dap_protocol/include
        ../../../components/usb_device/private_include
)
target_compile_features(dap_stream_test PRIVATE c_std_11)
target_compile_options(dap_stream_test PRIVATE -Wall -Wextra -Werror -pedantic)

add_test(NAME dap_stream_recovery COMMAND dap_stream_test)
