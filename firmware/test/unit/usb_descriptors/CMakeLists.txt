cmake_minimum_required(VERSION 3.16)

project(airdap_usb_descriptors_test C)

enable_testing()

function(add_usb_descriptors_test target debug_shell_enabled)
    add_executable(
        ${target}
        test_usb_descriptors.c
        ../../../components/usb_device/usb_descriptors.c
    )
    target_include_directories(
        ${target}
        PRIVATE
            fake_include
            ../../../components/usb_device/private_include
            ../../../managed_components/espressif__tinyusb/src
    )
    target_compile_definitions(
        ${target}
        PRIVATE CONFIG_AIRDAP_DEBUG_SHELL=${debug_shell_enabled}
    )
    target_compile_features(${target} PRIVATE c_std_11)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -pedantic)
    add_test(NAME ${target} COMMAND ${target})
endfunction()

add_usb_descriptors_test(usb_descriptors_default_test 0)
add_usb_descriptors_test(usb_descriptors_debug_shell_test 1)
