cmake_minimum_required(VERSION 3.16)

project(airdap_debug_shell_input_test C)

enable_testing()

add_executable(
    debug_shell_input_test
    test_debug_shell_input.c
    ../../../components/usb_device/debug_shell_input.c
)

target_include_directories(
    debug_shell_input_test
    PRIVATE ../../../components/usb_device/private_include
)
target_compile_features(debug_shell_input_test PRIVATE c_std_11)
target_compile_options(debug_shell_input_test PRIVATE -Wall -Wextra -Werror -pedantic)

add_test(NAME debug_shell_bounded_input COMMAND debug_shell_input_test)
