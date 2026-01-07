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

add_test( NAME stage03_queue_push_for_timeout_when_full
  COMMAND ops_tests --filter="Stage03 Queue: push_for times out when queue is full"
)

add_test( NAME stage03_queue_wait_pop_blocks_until_push
  COMMAND ops_tests --filter="Stage03 Queue: wait_pop blocks until push then returns element"
)

add_test( NAME stage03_queue_wait_pop_for_timeout_when_empty
  COMMAND ops_tests --filter="Stage03 Queue: wait_pop_for times out when empty"
)

add_test( NAME stage03_queue_close_wakes_wait_pop
  COMMAND ops_tests --filter="Stage03 Queue: close wakes wait_pop and it returns false when empty"
)

add_test( NAME stage03_queue_close_wakes_waiting_push
  COMMAND ops_tests --filter="Stage03 Queue: close wakes push that waits for space and push returns false"
)

add_test( NAME stage03_queue_push_returns_false_after_close
  COMMAND ops_tests --filter="Stage03 Queue: push returns false after close"
)

add_test( NAME stage03_queue_no_spin_practical
  COMMAND ops_tests --filter="Stage03 Queue: no spin in wait_pop_for (practical anti-busy-wait)"
)

add_test( NAME stage03_pipeline_start_twice_throws
  COMMAND ops_tests --filter="Stage03 Pipeline: start called twice throws logic_error"
)

add_test( NAME stage03_pipeline_submit_before_start_allowed
  COMMAND ops_tests --filter="Stage03 Pipeline: submit before start is allowed (then start+shutdown delivers)"
)

add_test( NAME stage03_pipeline_submit_backpressure_deterministic_before_start
  COMMAND ops_tests --filter="Stage03 Pipeline: submit backpressure timeout is deterministic before start"
)

add_test( NAME stage03_pipeline_shutdown_strict_invariants
  COMMAND ops_tests --filter="Stage03 Pipeline: shutdown delivers all accepted and satisfies strict invariants"
)

add_test( NAME stage03_pipeline_submit_after_shutdown_throws
  COMMAND ops_tests --filter="Stage03 Pipeline: submit after shutdown throws runtime_error"
)

add_test( NAME stage03_pipeline_stable_after_shutdown
  COMMAND ops_tests --filter="Stage03 Pipeline: metrics and delivered are stable after shutdown"
)

add_test( NAME stage03_pipeline_cancel_completes_quickly
  COMMAND ops_tests --filter="Stage03 Pipeline: cancel completes quickly (no deadlock)"
)

add_test( NAME stage03_pipeline_destructor_safe
  COMMAND ops_tests --filter="Stage03 Pipeline: destructor is safe without explicit shutdown/cancel"
)