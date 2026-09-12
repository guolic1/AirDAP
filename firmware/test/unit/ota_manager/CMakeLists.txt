cmake_minimum_required(VERSION 3.16)

project(airdap_ota_manager_test C)

enable_testing()
find_package(Threads REQUIRED)

add_executable(
    ota_manager_test
    test_ota_manager.c
    ../../../components/network_ota/network_ota.c
    ../../../components/dap_ownership/dap_ownership.c
    ../../../components/mode_state/mode_state.c
    ../../../components/ota_manager/ota_manager.c
)
target_include_directories(
    ota_manager_test
    PRIVATE
        fake_include
        ../../../components/network_ota/include
        ../../../components/network_auth/include
        ../../../components/airdap_frame/include
        ../target_uart/fake_include
        ../../../components/target_uart/include
        ../../../components/dap_ownership/include
        ../../../components/mode_state/include
        ../../../components/ota_manager/include
)
target_compile_features(ota_manager_test PRIVATE c_std_11)
target_compile_options(ota_manager_test PRIVATE -Wall -Wextra -Werror -pedantic)

add_test(NAME ota_manager_state COMMAND ota_manager_test)

target_link_libraries(ota_manager_test PRIVATE Threads::Threads)
