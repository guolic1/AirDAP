cmake_minimum_required(VERSION 3.16)

set(IDF_TARGET "esp32s3")

get_filename_component(
    AIRDAP_COMPONENTS
    "${CMAKE_CURRENT_LIST_DIR}/../../../components"
    ABSOLUTE
)
set(
    EXTRA_COMPONENT_DIRS
    "${AIRDAP_COMPONENTS}/config_store"
    "${AIRDAP_COMPONENTS}/dap_ownership"
    "${AIRDAP_COMPONENTS}/mode_state"
    "${AIRDAP_COMPONENTS}/wifi_manager"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(airdap_wifi_manager_hil)
