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

add_test(NAME stage02_blocking_queue_basic
         COMMAND ops_tests --filter="BlockingQueue: push/pop")

add_test(NAME stage02_blocking_queue_close_unblock
         COMMAND ops_tests --filter="unblocks after close")

add_test(NAME stage02_blocking_queue_push_after_close
         COMMAND ops_tests --filter="push returns false after close")

add_test(NAME stage02_blocking_queue_drain_exactly_once
         COMMAND ops_tests --filter="drain all items exactly once")

add_test(NAME stage02_blocking_queue_close_wakes_all
         COMMAND ops_tests --filter="close wakes all waiting consumers")

add_test(NAME stage02_pipeline_empty
         COMMAND ops_tests --filter="process_all does nothing for empty batch")

add_test(NAME stage02_pipeline_submit
         COMMAND ops_tests --filter="submit increments accepted_count")

add_test(NAME stage02_pipeline_invariants
         COMMAND ops_tests --filter="preserves timestamp invariants")

add_test(NAME stage02_pipeline_order
         COMMAND ops_tests --filter="matches submission order by id")

add_test(NAME stage02_pipeline_metrics_cumulative
         COMMAND ops_tests --filter="metrics are cumulative")

add_test(NAME stage02_pipeline_metrics_sum
         COMMAND ops_tests --filter="total_processing_time equals sum")

add_test(NAME stage02_pipeline_stress
         COMMAND ops_tests --filter="stability stress")

add_test(NAME stage02_pipeline_threads 
         COMMAND ops_tests --filter="uses at least two different threads")