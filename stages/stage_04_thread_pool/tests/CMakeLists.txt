add_executable(ops_tests
  test_main.cpp
  test_stage04_full.cpp
)

target_link_libraries(ops_tests PRIVATE
  ops_solution
)

target_apply_warnings(ops_tests)
target_enable_sanitizers(ops_tests)

target_include_directories(ops_tests PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/../../../cmake"
)

add_test( NAME stage04_order_strict_transitions
  COMMAND ops_tests --filter="Stage04: Order advance_to only allows strict step transitions"
)

add_test( NAME stage04_pipeline_initial_state_created
  COMMAND ops_tests --filter="Stage04: initial state is Created, not running, not stopped"
)

add_test( NAME stage04_pipeline_start_running_idempotent
  COMMAND ops_tests --filter="Stage04: start transitions to Running and is idempotent in Running"
)

add_test( NAME stage04_pipeline_start_in_stopped_throws
  COMMAND ops_tests --filter="Stage04: start in Stopped throws logic_error"
)

add_test( NAME stage04_pipeline_submit_only_running
  COMMAND ops_tests --filter="Stage04: submit returns false if not Running; true in Running"
)

add_test( NAME stage04_pipeline_graceful_shutdown_strict_metrics
  COMMAND ops_tests --filter="Stage04: graceful shutdown drains all accepted orders and makes metrics consistent"
)

add_test( NAME stage04_pipeline_shutdown_idempotent_metrics_stable
  COMMAND ops_tests --filter="Stage04: shutdown is idempotent and does not change final metrics"
)

add_test( NAME stage04_pipeline_metrics_monotonic_snapshots
  COMMAND ops_tests --filter="Stage04: metrics are monotonic under load (snapshots)"
)

add_test( NAME stage04_pipeline_shutdown_now_unblocks_producers
  COMMAND ops_tests --filter="Stage04: shutdown_now unblocks producers and stops accepting"
)

add_test( NAME stage04_pipeline_backpressure_tiny_cfg
  COMMAND ops_tests --filter="Stage04: backpressure triggers with tiny capacities and short timeout"
)

add_test( NAME stage04_pipeline_backpressure_accounting_conditional
  COMMAND ops_tests --filter="Stage04: backpressure accounting - if submit rejects, submit_timeout_count must increase"
)

add_test( NAME stage04_pipeline_readonly_threadsafe_under_load
  COMMAND ops_tests --filter="Stage04: concurrent read-only calls are safe during heavy submit load"
)

add_test( NAME stage04_pipeline_destructor_no_hang_under_overload
  COMMAND ops_tests --filter="Stage04: destructor does not hang under overload (implicit shutdown_now)"
)