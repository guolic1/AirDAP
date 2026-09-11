cmake_minimum_required(VERSION 3.16)
project(network_control_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_executable(network_control_test
    test_network_control.c
    ../../../components/network_control/network_control.c
    ../../../components/mode_state/mode_state.c
    ../../../components/dap_ownership/dap_ownership.c
)
target_include_directories(network_control_test PRIVATE
    ../board/fake_include
    ../../../components/network_control/include
    ../../../components/network_auth/include
    ../../../components/board/include
    ../../../components/airdap_frame/include
    ../../../components/mode_state/include
    ../../../components/dap_ownership/include
)
target_compile_options(network_control_test PRIVATE -Wall -Wextra -Werror -pedantic)
enable_testing()
add_test(NAME network_control_contract COMMAND network_control_test)
