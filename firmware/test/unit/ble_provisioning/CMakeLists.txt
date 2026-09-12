cmake_minimum_required(VERSION 3.16)

project(airdap_ble_provisioning_test C)

enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_executable(
    ble_provisioning_test
    test_ble_provisioning.c
    ../../../components/ble_provisioning/provisioning_button.c
    ../../../components/ble_provisioning/sec2_credentials.c
)
target_include_directories(
    ble_provisioning_test
    PRIVATE
        fake_include
        ../../../components/ble_provisioning/include
        ../../../components/ble_provisioning/private_include
)
target_compile_features(ble_provisioning_test PRIVATE c_std_11)
target_compile_options(ble_provisioning_test PRIVATE -Wall -Wextra -Werror -pedantic)

add_test(NAME ble_provisioning_contract COMMAND ble_provisioning_test)
add_executable(button_state_machine_test
    test_button_state_machine.c
    ../../../components/ble_provisioning/provisioning_button.c)
target_include_directories(button_state_machine_test PRIVATE
    ../../../components/ble_provisioning/private_include)
target_compile_features(button_state_machine_test PRIVATE c_std_11)
target_compile_options(button_state_machine_test PRIVATE -Wall -Wextra -Werror -pedantic)
add_test(NAME button_state_machine COMMAND button_state_machine_test)
add_executable(button_indicator_test
    test_button_indicator.c
    ../../../components/ble_provisioning/button_indicator.c)
target_include_directories(button_indicator_test PRIVATE
    ../../../components/ble_provisioning/private_include)
target_compile_features(button_indicator_test PRIVATE c_std_11)
target_compile_options(button_indicator_test PRIVATE -Wall -Wextra -Werror -pedantic)
add_test(NAME button_indicator COMMAND button_indicator_test)
add_test(
    NAME ble_provisioning_public_security2_credential
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_public_sec2_credential.py
)

add_executable(
    ble_provisioning_adapter_test
    test_ble_provisioning_adapter.c
    ../debug_shell_button/fake_button_config.c
    ../../../components/ble_provisioning/button_indicator.c
    ../../../components/ble_provisioning/ble_provisioning.c
    ../../../components/ble_provisioning/button_simulation.c
    ../../../components/ble_provisioning/provisioning_button.c
)
target_include_directories(
    ble_provisioning_adapter_test
    PRIVATE
        ../../../components/button_config/include
        ../../../components/mode_state/include
        ../../../components/dap_ownership/include
        fake_include
        ../../../components/ble_provisioning/include
        ../../../components/ble_provisioning/private_include
)
target_compile_features(ble_provisioning_adapter_test PRIVATE c_std_11)
target_compile_options(
    ble_provisioning_adapter_test
    PRIVATE -Wall -Wextra -Werror -pedantic
)
target_compile_definitions(
    ble_provisioning_adapter_test
    PRIVATE AIRDAP_BLE_PROVISIONING_TESTING=1
)

add_test(NAME ble_provisioning_adapter COMMAND ble_provisioning_adapter_test)

add_executable(button_device_commands_test test_button_device_commands.c
    ../../../components/ble_provisioning/button_device_commands.c
    ../../../components/board/board.c
    ../../../components/mode_state/mode_state.c
    ../../../components/dap_ownership/dap_ownership.c)
target_include_directories(button_device_commands_test PRIVATE
    ../../../components/ble_provisioning/private_include
    ../../../components/button_config/include
    ../../../components/board/include
    ../../../components/mode_state/include
    ../../../components/dap_ownership/include
    ../../../components/wifi_manager/include
    fake_include ../board/fake_include)
target_compile_features(button_device_commands_test PRIVATE c_std_11)
target_compile_options(button_device_commands_test PRIVATE -Wall -Wextra -Werror -pedantic)
add_test(NAME button_device_commands COMMAND button_device_commands_test)
