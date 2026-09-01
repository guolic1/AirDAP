cmake_minimum_required(VERSION 3.16)

project(airdap_dap_backend_test C)

enable_testing()

add_executable(
    dap_backend_test
    test_dap_backend.c
    ../../../components/dap_ownership/dap_ownership.c
    ../../../components/mode_state/mode_state.c
    ../../../components/dap_protocol/dap_backend.c
    ../../../components/dap_protocol/dap_protocol.c
)
target_include_directories(
    dap_backend_test
    PRIVATE
        fake_include
        ../../../components/dap_ownership/include
        ../../../components/mode_state/include
        ../../../components/dap_protocol/include
)
target_compile_features(dap_backend_test PRIVATE c_std_11)
target_compile_options(
    dap_backend_test
    PRIVATE -Wall -Wextra -Werror -pedantic
)

add_test(NAME dap_backend_ownership COMMAND dap_backend_test)
