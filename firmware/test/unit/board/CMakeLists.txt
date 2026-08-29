cmake_minimum_required(VERSION 3.16)

project(airdap_board_test C)

enable_testing()

add_executable(
    board_test
    test_board.c
    ../../../components/board/board.c
)

target_include_directories(
    board_test
    PRIVATE
        fake_include
        ../../../components/board/include
)
target_compile_features(board_test PRIVATE c_std_11)
target_compile_options(board_test PRIVATE -Wall -Wextra -Werror -pedantic)

add_test(NAME board_safe_gpio_state COMMAND board_test)
