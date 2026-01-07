cmake_minimum_required(VERSION 3.20)

project(order_pipeline_stage LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake")

include(compiler_options)
include(warnings)
include(sanitizer_options)
include(testing)

add_subdirectory(solution)
add_subdirectory(tests)