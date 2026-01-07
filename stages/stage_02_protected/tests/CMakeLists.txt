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

add_test( NAME stage02_queue_wait_pop_blocks
  COMMAND ops_tests --filter="Stage02 Queue: wait_pop blocks until push then returns element"
)

add_test( NAME stage02_queue_wait_pop_for_timeout
  COMMAND ops_tests --filter="Stage02 Queue: wait_pop_for times out when no data"
)

add_test( NAME stage02_queue_close_wakes_wait_pop
  COMMAND ops_tests --filter="Stage02 Queue: close wakes wait_pop and it returns false when empty"
)

add_test( NAME stage02_queue_push_after_close_throws
  COMMAND ops_tests --filter="Stage02 Queue: push after close throws logic_error"
)

add_test( NAME stage02_queue_close_idempotent
  COMMAND ops_tests --filter="Stage02 Queue: close is idempotent"
)

add_test( NAME stage02_queue_no_spin_practical
  COMMAND ops_tests --filter="Stage02 Queue: wait_pop_for does not spin (practical anti-busy-wait)"
)

add_test( NAME stage02_pipeline_basic_flow
  COMMAND ops_tests --filter="Stage02 Pipeline: basic flow delivers all orders and metrics match strictly"
)

add_test( NAME stage02_pipeline_concurrent_submit
  COMMAND ops_tests --filter="Stage02 Pipeline: concurrent submit from multiple threads"
)

add_test( NAME stage02_pipeline_shutdown_completes
  COMMAND ops_tests --filter="Stage02 Pipeline: shutdown completes (no deadlock)"
)

add_test( NAME stage02_pipeline_shutdown_idempotent_and_stable
  COMMAND ops_tests --filter="Stage02 Pipeline: shutdown is idempotent and state is stable after shutdown"
)

add_test( NAME stage02_pipeline_submit_after_shutdown_rejected
  COMMAND ops_tests --filter="Stage02 Pipeline: submit after shutdown is rejected"
)

add_test( NAME stage02_pipeline_total_lead_time_sum
  COMMAND ops_tests --filter="Stage02 Pipeline: total_lead_time equals sum(delivered_time - accepted_time)"
)

add_test( NAME stage02_pipeline_destructor_safe
  COMMAND ops_tests --filter="Stage02 Pipeline: destructor is safe without explicit shutdown"
)