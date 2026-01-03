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

add_test(NAME stage04_bounded_queue_basic
         COMMAND ops_tests --filter="BoundedBlockingQueue: push/pop")

add_test(NAME stage04_bounded_queue_close_unblock
         COMMAND ops_tests --filter="BoundedBlockingQueue: close wakes waiters")

add_test(NAME stage04_bounded_queue_push_after_close
         COMMAND ops_tests --filter="BoundedBlockingQueue: push returns false after close")

add_test(NAME stage04_bounded_queue_pop_nullopt_after_close_empty
         COMMAND ops_tests --filter="BoundedBlockingQueue: pop returns nullopt when closed and empty")

add_test(NAME stage04_bounded_queue_backpressure_blocks
         COMMAND ops_tests --filter="BoundedBlockingQueue: push_blocking blocks when full and continues after pop")

add_test(NAME stage04_bounded_queue_pop_blocks
         COMMAND ops_tests --filter="BoundedBlockingQueue: pop_blocking blocks when empty and unblocks on push")

add_test(NAME stage04_pipeline_empty
         COMMAND ops_tests --filter="process_all on empty pipeline returns immediately")

add_test(NAME stage04_pipeline_barrier_snapshot
         COMMAND ops_tests --filter="process_all is a barrier for orders accepted before call")

add_test(NAME stage04_pipeline_repeated_process_all
         COMMAND ops_tests --filter="repeated process_all accumulates delivered and metrics")

add_test(NAME stage04_pipeline_concurrent_submit
         COMMAND ops_tests --filter="concurrent submit + barrier completes and produces all orders")

add_test(NAME stage04_pipeline_snapshot_while_submitting
         COMMAND ops_tests --filter="process_all waits only for snapshot of accepted orders while submits continue")

add_test(NAME stage04_pipeline_stress_concurrent_submit
         COMMAND ops_tests --filter="stress concurrent submit")

add_test(NAME stage04_pipeline_repeated_scenario
         COMMAND ops_tests --filter="repeated scenario run does not hang")

add_test(NAME stage04_pipeline_destructor_pending_orders
         COMMAND ops_tests --filter="destruction does not hang with pending orders")

add_test(NAME stage04_pipeline_destructor_stress
         COMMAND ops_tests --filter="destructor stress")

add_test(NAME stage04_pipeline_threads
         COMMAND ops_tests --filter="uses more than one worker thread")