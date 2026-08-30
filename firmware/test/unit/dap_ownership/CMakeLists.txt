cmake_minimum_required(VERSION 3.16)

project(airdap_dap_ownership_test C)

enable_testing()

add_executable(
    dap_ownership_test
    test_dap_ownership.c
    ../../../components/dap_ownership/dap_ownership.c
)
target_include_directories(
    dap_ownership_test
    PRIVATE ../../../components/dap_ownership/include
)
target_compile_features(dap_ownership_test PRIVATE c_std_11)
target_compile_options(
    dap_ownership_test
    PRIVATE -Wall -Wextra -Werror -pedantic
)

add_test(NAME dap_ownership_state COMMAND dap_ownership_test)
