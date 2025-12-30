add_executable(ops_tests
  test_main.cpp
)

target_link_libraries(ops_tests PRIVATE ops_solution)

target_apply_warnings(ops_tests)
target_enable_sanitizers(ops_tests)

target_include_directories(ops_tests PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/../../../cmake"
)

add_test(NAME ops_tests COMMAND ops_tests)