cmake_minimum_required(VERSION 3.16)

project(airdap_mode_state_test C)

enable_testing()

add_executable(
    mode_state_test
    test_mode_state.c
    ../../../components/mode_state/mode_state.c
)
target_include_directories(
    mode_state_test
    PRIVATE
        ../../../components/dap_ownership/include
        ../../../components/mode_state/include
)
target_compile_features(mode_state_test PRIVATE c_std_11)
target_compile_options(
    mode_state_test
    PRIVATE -Wall -Wextra -Werror -pedantic
)

add_test(NAME mode_state_transitions COMMAND mode_state_test)
