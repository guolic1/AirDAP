cmake_minimum_required(VERSION 3.16)

project(airdap_usb_uart_bridge_test C)

enable_testing()

add_executable(
    usb_uart_bridge_test
    test_usb_uart_bridge.c
    ../../../components/usb_device/usb_uart_bridge.c
)

target_include_directories(
    usb_uart_bridge_test
    PRIVATE
        fake_include
        ../../../components/target_uart/include
        ../../../components/usb_device/private_include
)
target_compile_features(usb_uart_bridge_test PRIVATE c_std_11)
target_compile_options(
    usb_uart_bridge_test
    PRIVATE -Wall -Wextra -Werror -pedantic
)

add_test(NAME usb_uart_bridge_lifecycle COMMAND usb_uart_bridge_test)
