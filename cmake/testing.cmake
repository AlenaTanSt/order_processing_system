include(CTest)
enable_testing()

set(CTEST_TEST_TIMEOUT 20 CACHE STRING "Default test timeout (seconds)")

add_custom_target(check
  COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
  WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
)
