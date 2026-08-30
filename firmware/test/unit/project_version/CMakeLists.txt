cmake_minimum_required(VERSION 3.22)

project(airdap_project_version_tests NONE)

enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_test(
    NAME project_version_resolution
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_project_version.py
)
