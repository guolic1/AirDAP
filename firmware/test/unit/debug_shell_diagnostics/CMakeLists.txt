cmake_minimum_required(VERSION 3.16)

project(airdap_debug_shell_diagnostics_test C)

enable_testing()
find_package(Threads REQUIRED)

add_executable(
    debug_shell_diagnostics_test
    test_debug_shell_diagnostics.c
    ../../../components/usb_device/debug_shell_commands.c
    ../../../components/usb_device/debug_shell_config_status.c
    ../../../components/usb_device/debug_shell_core_commands.c
    ../../../components/usb_device/debug_shell_diagnostics.c
    ../../../components/usb_device/debug_shell_identity.c
    ../../../components/usb_device/debug_shell_service_diagnostics.c
)
target_include_directories(
    debug_shell_diagnostics_test
    PRIVATE
        fake_include
        ../../../components/board/include
        ../../../components/config_store/include
        ../../../components/dap_ownership/include
        ../../../components/dap_service/include
        ../../../components/device_identity/include
        ../../../components/discovery/include
        ../../../components/mode_state/include
        ../../../components/network_auth/include
        ../../../components/network_dap/include
        ../../../components/ota_manager/include
        ../../../components/target_uart/include
        ../../../components/usb_device/private_include
        ../../../components/voltage_monitor/include
        ../../../components/wifi_manager/include
)
target_compile_features(debug_shell_diagnostics_test PRIVATE c_std_11)
target_link_libraries(debug_shell_diagnostics_test PRIVATE Threads::Threads)
target_compile_options(
    debug_shell_diagnostics_test
    PRIVATE -Wall -Wextra -Werror -pedantic
)

add_test(NAME debug_shell_diagnostics COMMAND debug_shell_diagnostics_test)
