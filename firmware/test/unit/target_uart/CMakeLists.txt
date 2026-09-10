cmake_minimum_required(VERSION 3.16)

project(airdap_target_uart_test C)

enable_testing()
find_package(Threads REQUIRED)

add_executable(
    target_uart_test
    test_target_uart.c
    ../../../components/target_uart/target_uart.c
)

target_include_directories(
    target_uart_test
    PRIVATE
        fake_include
        ../../../components/board/include
        ../../../components/target_uart/include
        ../../../components/target_uart/private_include
)
target_compile_features(target_uart_test PRIVATE c_std_11)
target_compile_options(target_uart_test PRIVATE -Wall -Wextra -Werror -pedantic)
target_link_libraries(target_uart_test PRIVATE Threads::Threads)

add_test(NAME target_uart_configuration COMMAND target_uart_test)
