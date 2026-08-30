cmake_minimum_required(VERSION 3.16)

project(airdap_debug_shell_config_status_test C)

enable_testing()

add_executable(
    debug_shell_config_status_test
    test_debug_shell_config_status.c
    ../../../components/usb_device/debug_shell_config_status.c
)
target_include_directories(
    debug_shell_config_status_test
    PRIVATE
        fake_include
        ../../../components/config_store/include
        ../../../components/usb_device/private_include
)
target_compile_features(debug_shell_config_status_test PRIVATE c_std_11)
target_compile_options(
    debug_shell_config_status_test
    PRIVATE -Wall -Wextra -Werror -pedantic
)

add_test(NAME debug_shell_config_status COMMAND debug_shell_config_status_test)
