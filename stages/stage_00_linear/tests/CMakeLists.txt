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
         COMMAND ops_tests --filter="Empty pipeline: process_all does not change metrics")

add_test(NAME stage00_submit_increases_accepted
         COMMAND ops_tests --filter="Submit increases accepted_count")

add_test(NAME stage00_linear_processing_metrics
         COMMAND ops_tests --filter="Linear processing: all orders are delivered and metrics are correct")

add_test(NAME stage00_order_preserved
         COMMAND ops_tests --filter="The order is preserved: delivered_orders go in the same id order")

add_test(NAME stage00_statuses_and_timestamps
         COMMAND ops_tests --filter="Statuses and timestamps: Delivered is set, timestamps are monotonous")

add_test(NAME stage00_total_processing_time_sum
         COMMAND ops_tests --filter="total_processing_time is equal to the sum of orders")

add_test(NAME stage00_process_all_idempotent
         COMMAND ops_tests --filter="Calling process_all again without new orders does not change the result.")