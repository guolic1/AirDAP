cmake_minimum_required(VERSION 3.16)

project(airdap_dap_service_test C)

enable_testing()
find_package(Threads REQUIRED)

add_executable(
    dap_service_test
    test_dap_service.c
    ../../../components/dap_service/dap_service.c
)
target_include_directories(
    dap_service_test
    PRIVATE
        fake_include
        ../../../components/dap_ownership/include
        ../../../components/dap_protocol/include
        ../../../components/dap_service/include
        ../../../components/dap_service/private_include
)
target_compile_features(dap_service_test PRIVATE c_std_11)
target_compile_options(
    dap_service_test
    PRIVATE -Wall -Wextra -Werror -pedantic
)
target_link_libraries(dap_service_test PRIVATE Threads::Threads)

add_test(NAME dap_service_routing COMMAND dap_service_test)
