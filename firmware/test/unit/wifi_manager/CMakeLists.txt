cmake_minimum_required(VERSION 3.16)

project(airdap_wifi_manager_test C)

enable_testing()

add_executable(
    wifi_manager_test
    test_wifi_manager.c
    ../../../components/wifi_manager/wifi_credentials.c
    ../../../components/wifi_manager/wifi_disconnect_reason.c
    ../../../components/wifi_manager/wifi_state_machine.c
)
target_include_directories(
    wifi_manager_test
    PRIVATE
        fake_include
        ../../../components/wifi_manager/include
        ../../../components/wifi_manager/private_include
)
target_compile_features(wifi_manager_test PRIVATE c_std_11)
target_compile_options(wifi_manager_test PRIVATE -Wall -Wextra -Werror -pedantic)

add_test(NAME wifi_manager_state_machine COMMAND wifi_manager_test)

add_executable(
    wifi_manager_adapter_test
    test_wifi_manager_adapter.c
    ../../../components/wifi_manager/wifi_credentials.c
    ../../../components/wifi_manager/wifi_disconnect_reason.c
    ../../../components/wifi_manager/wifi_manager.c
    ../../../components/wifi_manager/wifi_state_machine.c
)
target_include_directories(
    wifi_manager_adapter_test
    PRIVATE
        fake_include
        ../../../components/wifi_manager/include
        ../../../components/wifi_manager/private_include
)
target_compile_features(wifi_manager_adapter_test PRIVATE c_std_11)
target_compile_options(
    wifi_manager_adapter_test
    PRIVATE -Wall -Wextra -Werror -pedantic
)

add_test(NAME wifi_manager_adapter COMMAND wifi_manager_adapter_test)
