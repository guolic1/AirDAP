cmake_minimum_required(VERSION 3.16)
project(usb_lifecycle_test C)
enable_testing()
foreach(debug_shell 0 1)
    set(target usb_lifecycle_${debug_shell})
    add_executable(${target} test_usb_lifecycle.c
        ../../../components/usb_device/usb_device.c
        ../../../components/usb_device/usb_descriptors.c)
    target_compile_definitions(${target} PRIVATE CONFIG_AIRDAP_DEBUG_SHELL=${debug_shell})
    target_compile_features(${target} PRIVATE c_std_11)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
    target_include_directories(${target} PRIVATE fake_include
        ../usb_descriptors/fake_include
        ../../../managed_components/espressif__tinyusb/src
        ../../../components/usb_device/include ../../../components/usb_device/private_include
        ../../../components/dap_protocol/include ../../../components/dap_service/include
        ../../../components/device_identity/include ../../../components/mode_state/include
        ../../../components/dap_ownership/include ../network_dap/fake_include)
    add_test(NAME ${target} COMMAND ${target})
endforeach()
