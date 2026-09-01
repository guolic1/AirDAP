cmake_minimum_required(VERSION 3.16)

project(airdap_frame_test C)

enable_testing()

add_executable(
    airdap_frame_test
    test_airdap_frame.c
    ../../../components/airdap_frame/airdap_frame.c
)
target_include_directories(
    airdap_frame_test
    PRIVATE ../../../components/airdap_frame/include
)
target_compile_features(airdap_frame_test PRIVATE c_std_11)
target_compile_options(
    airdap_frame_test
    PRIVATE -Wall -Wextra -Werror -pedantic
)

add_test(NAME airdap_frame_contract COMMAND airdap_frame_test)
