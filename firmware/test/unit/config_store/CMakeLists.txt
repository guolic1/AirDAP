cmake_minimum_required(VERSION 3.16)

project(airdap_config_store_test C)

enable_testing()
find_package(Threads REQUIRED)

add_executable(
    config_store_test
    test_config_store.c
    ../../../components/config_store/config_store.c
)
target_include_directories(
    config_store_test
    PRIVATE
        fake_include
        ../../../components/config_store/include
        ../../../components/config_store/private_include
)
target_compile_features(config_store_test PRIVATE c_std_11)
target_compile_options(config_store_test PRIVATE -Wall -Wextra -Werror -pedantic)
target_compile_definitions(config_store_test PRIVATE AIRDAP_CONFIG_STORE_TESTING=1)
target_link_libraries(config_store_test PRIVATE Threads::Threads)

add_test(NAME config_store_contract COMMAND config_store_test)
