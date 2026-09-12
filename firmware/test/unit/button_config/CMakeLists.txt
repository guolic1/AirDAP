cmake_minimum_required(VERSION 3.16)
project(airdap_button_config_test C)
enable_testing()
find_package(Threads REQUIRED)
add_executable(button_config_test test_button_config.c
    ../../../components/button_config/button_config.c
    ../../../components/usb_device/debug_shell_button.c
    ../../../components/usb_device/debug_shell_commands.c)
target_include_directories(button_config_test PRIVATE fake_include
    ../config_store/fake_include ../../../components/board/include
    ../../../components/button_config/include
    ../../../components/ble_provisioning/include
    ../../../components/mode_state/include
    ../../../components/dap_ownership/include
    ../../../components/usb_device/private_include)
target_compile_features(button_config_test PRIVATE c_std_11)
target_compile_options(button_config_test PRIVATE -Wall -Wextra -Werror -pedantic)
target_compile_definitions(button_config_test PRIVATE AIRDAP_BUTTON_CONFIG_TESTING=1)
target_link_libraries(button_config_test PRIVATE Threads::Threads)
add_test(NAME button_config_persistence_and_shell COMMAND button_config_test)
