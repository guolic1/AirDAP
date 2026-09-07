cmake_minimum_required(VERSION 3.16)

project(airdap_debug_shell_button_test C)

enable_testing()

add_executable(
    debug_shell_button_test
    test_debug_shell_button.c
    ../../../components/usb_device/debug_shell_button.c
    ../../../components/usb_device/debug_shell_commands.c
)
target_include_directories(
    debug_shell_button_test
    PRIVATE
        fake_include
        ../../../components/usb_device/private_include
)
target_compile_features(debug_shell_button_test PRIVATE c_std_11)
target_compile_options(
    debug_shell_button_test
    PRIVATE -Wall -Wextra -Werror -pedantic
)

add_test(NAME debug_shell_button COMMAND debug_shell_button_test)

add_executable(
    debug_shell_button_flow_test
    test_debug_shell_button_flow.c
    ../../../components/board/board.c
    ../../../components/ble_provisioning/provisioning_button.c
    ../../../components/usb_device/debug_shell_button.c
    ../../../components/usb_device/debug_shell_commands.c
)
target_include_directories(
    debug_shell_button_flow_test
    PRIVATE
        ../../../components/board/include
        fake_include
        ../board/fake_include
        ../../../components/ble_provisioning/private_include
        ../../../components/usb_device/private_include
)
target_compile_features(debug_shell_button_flow_test PRIVATE c_std_11)
target_compile_options(
    debug_shell_button_flow_test
    PRIVATE -Wall -Wextra -Werror -pedantic
)

add_test(NAME debug_shell_button_flow COMMAND debug_shell_button_flow_test)
