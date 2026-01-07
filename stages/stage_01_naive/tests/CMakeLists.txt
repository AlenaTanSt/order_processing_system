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

add_test( NAME stage01_worker_threads_used
    COMMAND ops_tests --filter="Stage01: worker_threads_used equals max(1, hardware_concurrency)"
)

add_test( NAME stage01_empty_pipeline
    COMMAND ops_tests --filter="Stage01: process_all on empty pipeline is allowed and stable"
)

add_test( NAME stage01_concurrent_submit
    COMMAND ops_tests --filter="Stage01: concurrent submit is safe and no orders are lost or duplicated"
)

add_test( NAME stage01_total_processing_time
    COMMAND ops_tests --filter="Stage01: delivered_orders size equals delivered_count and total_processing_time matches sum"
)

add_test( NAME stage01_metrics_stable
    COMMAND ops_tests --filter="Stage01: metrics are stable after process_all returns"
)

add_test( NAME stage01_idempotent
    COMMAND ops_tests --filter="Stage01: process_all is idempotent when queue is empty"
)

add_test( NAME stage01_submit_blocks
    COMMAND ops_tests --filter="Stage01: submit blocks while process_all is running"
)

add_test( NAME stage01_stress_random
    COMMAND ops_tests --filter="Stage01: stress multiple runs with random batch sizes"
)