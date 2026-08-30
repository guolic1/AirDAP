cmake_minimum_required(VERSION 3.16)

get_filename_component(
    AIRDAP_CONFIG_STORE_COMPONENT
    "${CMAKE_CURRENT_LIST_DIR}/../../../components/config_store"
    ABSOLUTE
)
set(EXTRA_COMPONENT_DIRS "${AIRDAP_CONFIG_STORE_COMPONENT}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(airdap_config_store_hil)
