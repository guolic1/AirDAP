cmake_minimum_required(VERSION 3.16)

project(airdap_ota_manager_test C)

enable_testing()

add_executable(
    ota_manager_test
    test_ota_manager.c
    ../../../components/ota_manager/ota_manager.c
)
target_include_directories(
    ota_manager_test
    PRIVATE
        fake_include
        ../../../components/ota_manager/include
)
target_compile_features(ota_manager_test PRIVATE c_std_11)
target_compile_options(ota_manager_test PRIVATE -Wall -Wextra -Werror -pedantic)

add_test(NAME ota_manager_state COMMAND ota_manager_test)
