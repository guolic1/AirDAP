cmake_minimum_required(VERSION 3.22)

set(IDF_TARGET "esp32s3")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Build only the components required by the application and its dependencies.
idf_build_set_property(MINIMAL_BUILD ON)

project(airdap)
