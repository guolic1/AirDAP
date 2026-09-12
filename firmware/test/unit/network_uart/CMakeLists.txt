cmake_minimum_required(VERSION 3.16)
project(network_uart_test C)
find_package(Threads REQUIRED)
add_executable(network_uart_test test_network_uart.c
    ../../../components/network_uart/network_uart.c
    ../../../components/target_uart/target_uart.c)
target_include_directories(network_uart_test PRIVATE
    ../target_uart/fake_include
    ../../../components/board/include
    ../../../components/target_uart/include
    ../../../components/target_uart/private_include
    ../../../components/network_uart/include
    ../../../components/network_auth/include
    ../../../components/airdap_frame/include)
target_compile_features(network_uart_test PRIVATE c_std_11)
target_compile_options(network_uart_test PRIVATE -Wall -Wextra -Werror -pedantic)
target_link_libraries(network_uart_test PRIVATE Threads::Threads)
enable_testing()
add_test(NAME network_uart_protocol COMMAND network_uart_test)
