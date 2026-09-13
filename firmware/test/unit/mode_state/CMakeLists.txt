cmake_minimum_required(VERSION 3.16)

project(airdap_mode_state_test C)

enable_testing()

add_executable(
    mode_state_test
    test_mode_state.c
    ../../../components/mode_state/mode_state.c
    fake_mode_storage.c
)
target_include_directories(
    mode_state_test
    PRIVATE
        ../../../components/dap_ownership/include
        ../../../components/mode_state/private_include
        ../../../components/mode_state/include
)
target_compile_features(mode_state_test PRIVATE c_std_11)
target_compile_options(
    mode_state_test
    PRIVATE -Wall -Wextra -Werror -pedantic
)

add_test(NAME mode_state_transitions COMMAND mode_state_test)

add_executable(mode_storage_test test_mode_storage.c
    ../../../components/mode_state/mode_storage.c)
target_include_directories(mode_storage_test PRIVATE fake_include
    ../config_store/fake_include
    ../../../components/mode_state/include
    ../../../components/mode_state/private_include
    ../../../components/dap_ownership/include)
target_compile_features(mode_storage_test PRIVATE c_std_11)
target_compile_options(mode_storage_test PRIVATE -Wall -Wextra -Werror -pedantic)
add_test(NAME mode_storage COMMAND mode_storage_test)
