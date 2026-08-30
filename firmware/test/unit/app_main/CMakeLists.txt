cmake_minimum_required(VERSION 3.16)

project(airdap_app_main_test C)

enable_testing()

add_executable(
    app_main_test
    test_app_main.c
    ../../../main/app_main.c
)
target_include_directories(app_main_test PRIVATE fake_include)
target_compile_features(app_main_test PRIVATE c_std_11)
target_compile_options(app_main_test PRIVATE -Wall -Wextra -Werror -pedantic)

add_test(NAME app_main_initialization_order COMMAND app_main_test)
