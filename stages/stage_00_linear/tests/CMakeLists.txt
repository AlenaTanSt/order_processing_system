add_executable(ops_tests
  test_main.cpp
)

target_link_libraries(ops_tests PRIVATE
  ops_solution
)

target_apply_warnings(ops_tests)
target_enable_sanitizers(ops_tests)

target_include_directories(ops_tests PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/../../../cmake"
)

add_test(NAME stage00_empty_pipeline
         COMMAND ops_tests --filter="Pipeline: process_all on empty pipeline does nothing")

add_test(NAME stage00_initial_state
         COMMAND ops_tests --filter="Order: initial state is Accepted and accepted_time set")

add_test(NAME stage00_valid_transitions
         COMMAND ops_tests --filter="Order: valid transitions set timestamps and keep previous ones")

add_test(NAME stage00_invalid_transition
         COMMAND ops_tests --filter="Order: invalid transition throws logic_error and does not mutate state")

add_test(NAME stage00_queue_fifo_and_pop_empty
         COMMAND ops_tests --filter="OrderQueue: FIFO and pop empty throws out_of_range")

add_test(NAME stage00_pipeline_processing_metrics
         COMMAND ops_tests --filter="Pipeline: processes all orders sequentially, preserves order, updates metrics")