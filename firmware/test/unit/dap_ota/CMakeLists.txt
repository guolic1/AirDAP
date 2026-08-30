cmake_minimum_required(VERSION 3.16)

project(airdap_dap_ota_test C)

enable_testing()

add_executable(
    dap_ota_test
    test_dap_ota.c
    ../../../components/dap_protocol/dap_ota.c
)
target_include_directories(
    dap_ota_test
    PRIVATE
        fake_include
        ../../../components/dap_protocol/private_include
)
target_compile_features(dap_ota_test PRIVATE c_std_11)
target_compile_options(dap_ota_test PRIVATE -Wall -Wextra -Werror -pedantic)

add_test(NAME dap_ota_commands COMMAND dap_ota_test)
