cmake_minimum_required(VERSION 3.16)

project(airdap_voltage_monitor_test C)

enable_testing()

add_executable(
    voltage_monitor_test
    test_voltage_monitor.c
    ../../../components/voltage_monitor/voltage_monitor.c
)

target_include_directories(
    voltage_monitor_test
    PRIVATE
        fake_include
        ../../../components/board/include
        ../../../components/voltage_monitor/include
)
target_compile_features(voltage_monitor_test PRIVATE c_std_11)
target_compile_options(voltage_monitor_test PRIVATE -Wall -Wextra -Werror -pedantic)

add_test(NAME voltage_monitor_conversion COMMAND voltage_monitor_test)
