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
add_test(
    NAME ble_provisioning_public_security2_credential
    COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_public_sec2_credential.py
)

add_executable(
    ble_provisioning_adapter_test
    test_ble_provisioning_adapter.c
    ../../../components/ble_provisioning/ble_provisioning.c
    ../../../components/ble_provisioning/provisioning_button.c
)
target_include_directories(
    ble_provisioning_adapter_test
    PRIVATE
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
