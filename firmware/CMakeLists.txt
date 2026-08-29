cmake_minimum_required(VERSION 3.22)

set(IDF_TARGET "esp32s3")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Build only the components required by the application and its dependencies.
idf_build_set_property(MINIMAL_BUILD ON)

project(airdap)

add_custom_target(airdap_verify_bootloader ALL
    COMMAND "${PYTHON}" "${CMAKE_CURRENT_LIST_DIR}/tools/verify_bootloader.py"
        --build-dir "${CMAKE_BINARY_DIR}"
        --nm "${CMAKE_NM}"
        --objdump "${CMAKE_OBJDUMP}"
    DEPENDS bootloader
    COMMENT "Verifying AirDAP bootloader artifact contract"
    VERBATIM
)
