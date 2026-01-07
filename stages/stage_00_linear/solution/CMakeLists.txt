add_library(ops_solution)
target_include_directories(ops_solution PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")

target_sources(ops_solution PRIVATE
  src/pipeline.cpp
)

target_apply_warnings(ops_solution)
target_enable_sanitizers(ops_solution)

add_executable(ops_app
  src/main.cpp
)

target_link_libraries(ops_app PRIVATE ops_solution)

target_apply_warnings(ops_app)
target_enable_sanitizers(ops_app)
