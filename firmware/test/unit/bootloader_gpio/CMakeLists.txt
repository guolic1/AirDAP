cmake_minimum_required(VERSION 3.16)

project(airdap_bootloader_gpio_test C)

enable_testing()

add_executable(
    bootloader_gpio_test
    test_bootloader_gpio.c
    ../../../bootloader_components/airdap_boot/hooks.c
)

target_include_directories(
    bootloader_gpio_test
    PRIVATE
        fake_include
        ../../../components/board/include
)
target_compile_features(bootloader_gpio_test PRIVATE c_std_11)
target_compile_options(bootloader_gpio_test PRIVATE -Wall -Wextra -Werror -pedantic)

add_test(NAME bootloader_gpio_safe_state COMMAND bootloader_gpio_test)
