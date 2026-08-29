cmake_minimum_required(VERSION 3.16)

project(airdap_swd_protocol_test C)

enable_testing()

add_executable(
    swd_protocol_test
    test_swd_protocol.c
    ../../../components/swd_engine/swd_protocol.c
)

target_include_directories(
    swd_protocol_test
    PRIVATE
        fake_include
        ../../../components/swd_engine/include
        ../../../components/swd_engine/private_include
)
target_compile_features(swd_protocol_test PRIVATE c_std_11)
target_compile_options(swd_protocol_test PRIVATE -Wall -Wextra -Werror -pedantic)

add_test(NAME swd_protocol_transactions COMMAND swd_protocol_test)
