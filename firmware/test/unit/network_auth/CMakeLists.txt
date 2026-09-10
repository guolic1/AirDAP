cmake_minimum_required(VERSION 3.16)

project(airdap_network_auth_test C)

enable_testing()
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

add_executable(
    network_auth_test
    test_network_auth.c
    ../../../components/network_auth/network_auth.c
)
target_include_directories(
    network_auth_test
    PRIVATE
        fake_include
        ../../../components/network_auth/include
)
target_compile_features(network_auth_test PRIVATE c_std_11)
target_compile_options(network_auth_test PRIVATE -Wall -Wextra -Werror -pedantic)
target_link_libraries(network_auth_test PRIVATE OpenSSL::Crypto Threads::Threads)

add_test(NAME network_auth_contract COMMAND network_auth_test)
add_test(NAME network_auth_persisted_record COMMAND network_auth_test --loaded-init)
add_test(NAME network_auth_malformed_record COMMAND network_auth_test --malformed-init)
add_test(
    NAME network_auth_uninitialized_status
    COMMAND network_auth_test --uninitialized-status
)
