cmake_minimum_required(VERSION 3.16)

project(network_dap_test C)

find_package(Threads REQUIRED)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(network_dap_test
    test_network_dap.c
    ../../../components/network_dap/network_dap.c
    ../../../components/airdap_frame/airdap_frame.c
)

target_include_directories(network_dap_test PRIVATE
    fake_include
    ../../../components/network_dap/include
    ../../../components/network_dap/private_include
    ../../../components/airdap_frame/include
    ../../../components/device_identity/include
    ../../../components/network_auth/include
)

target_compile_options(network_dap_test PRIVATE
    -Wall
    -Wextra
    -Werror
)
target_link_libraries(network_dap_test PRIVATE Threads::Threads)

enable_testing()
add_test(NAME network_dap_contract COMMAND network_dap_test)
add_test(
    NAME network_dap_uninitialized_status
    COMMAND network_dap_test --uninitialized-status
)
