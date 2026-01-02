add_executable(ops_tests
  test_main.cpp
)

target_link_libraries(ops_tests PRIVATE
  ops_solution
  stage_threads
)

target_compile_definitions(ops_tests PRIVATE OPS_TESTING)

target_apply_warnings(ops_tests)
target_enable_sanitizers(ops_tests)

target_include_directories(ops_tests PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/../../../cmake"
)

add_test(NAME stage01_empty_noop
         COMMAND ops_tests --filter="process_all: empty queue is a no-op")

add_test(NAME stage01_submit_metrics
         COMMAND ops_tests --filter="submit: increments accepted_count")

add_test(NAME stage01_one_order
         COMMAND ops_tests --filter="process_all: one order becomes Delivered")

add_test(NAME stage01_multi_order_ordering
         COMMAND ops_tests --filter="process_all: multiple orders preserve submit order")

add_test(NAME stage01_total_processing_time_sum
         COMMAND ops_tests --filter="total_processing_time: equals sum")

add_test(NAME stage01_cumulative_batches
         COMMAND ops_tests --filter="cumulative: two batches accumulate")

add_test(NAME stage01_idempotency
         COMMAND ops_tests --filter="idempotency: calling process_all twice")

add_test(NAME stage01_threads_unique_ids
         COMMAND ops_tests --filter="threads: for N >= 2")