cmake_minimum_required(VERSION 3.16)

project(airdap_discovery_test C)

enable_testing()

add_executable(
    discovery_test
    test_discovery.c
    ../../../components/discovery/discovery.c
)
target_include_directories(
    discovery_test
    PRIVATE
        fake_include
        ../../../components/discovery/include
        ../../../components/device_identity/include
)
target_compile_features(discovery_test PRIVATE c_std_11)
target_compile_options(discovery_test PRIVATE -Wall -Wextra -Werror -pedantic)

add_test(NAME discovery_lifecycle COMMAND discovery_test)
