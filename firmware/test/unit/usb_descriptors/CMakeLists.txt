cmake_minimum_required(VERSION 3.16)

project(airdap_usb_descriptors_test C)

enable_testing()

add_executable(
    usb_descriptors_test
    test_usb_descriptors.c
    ../../../components/usb_device/usb_descriptors.c
)

target_include_directories(
    usb_descriptors_test
    PRIVATE
        fake_include
        ../../../components/usb_device/private_include
        ../../../managed_components/espressif__tinyusb/src
)
target_compile_features(usb_descriptors_test PRIVATE c_std_11)
target_compile_options(usb_descriptors_test PRIVATE -Wall -Wextra -Werror -pedantic)

add_test(NAME usb_composite_descriptors COMMAND usb_descriptors_test)
