cmake_minimum_required(VERSION 3.16)

project(airdap_device_identity_test C)

enable_testing()

add_executable(
    device_identity_test
    test_device_identity.c
    ../../../components/device_identity/device_identity.c
)
target_include_directories(
    device_identity_test
    PRIVATE
        fake_include
        ../../../components/airdap_frame/include
        ../../../components/device_identity/include
        ../../../components/device_identity/private_include
)
target_compile_features(device_identity_test PRIVATE c_std_11)
target_compile_options(device_identity_test PRIVATE -Wall -Wextra -Werror -pedantic)

add_test(NAME device_identity_contract COMMAND device_identity_test)
