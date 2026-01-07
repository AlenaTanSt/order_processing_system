cmake_minimum_required(VERSION 3.20)

project(order_pipeline_stage LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake")

include(compiler_options)
include(warnings)
include(sanitizer_options)
include(testing)

find_package(Threads REQUIRED)

add_library(stage_threads INTERFACE)
target_link_libraries(stage_threads INTERFACE Threads::Threads)

add_subdirectory(solution)
add_subdirectory(tests)