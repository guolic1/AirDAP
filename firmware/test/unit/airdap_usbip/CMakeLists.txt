cmake_minimum_required(VERSION 3.16)
project(airdap_usbip_test NONE)

enable_testing()
find_package(Python3 3.13 REQUIRED COMPONENTS Interpreter)
add_test(NAME airdap_usbip_host_tool
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_airdap_usbip.py)
if(UNIX)
    add_test(NAME airdap_usbip_shutdown
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_shutdown.py)
endif()

if(WIN32)
    find_program(AIRDAP_POWERSHELL NAMES pwsh powershell REQUIRED)
    add_test(NAME airdap_usbip_background_task
        COMMAND ${AIRDAP_POWERSHELL} -NoProfile -NonInteractive -File
            ${CMAKE_CURRENT_SOURCE_DIR}/test_background.ps1
            -PythonPath ${Python3_EXECUTABLE})
endif()
