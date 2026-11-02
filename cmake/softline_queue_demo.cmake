# Shared example/fixture sources. Keep transport tests in hardening presets
# that disable the interactive examples; none of these targets are installed.
add_library(queue_demo_wire STATIC "${PROJECT_SOURCE_DIR}/examples/queue_demo_wire.c")
target_link_libraries(queue_demo_wire PUBLIC softline)
softline_apply_warnings(queue_demo_wire)
add_library(queue_demo_client STATIC "${PROJECT_SOURCE_DIR}/examples/queue_demo_client.c"
  "${PROJECT_SOURCE_DIR}/examples/queue_demo_timer.c"
  "${PROJECT_SOURCE_DIR}/examples/queue_demo_terminal.c"
  "${PROJECT_SOURCE_DIR}/src/softline_pty.c")
target_link_libraries(queue_demo_client PUBLIC queue_demo_wire)
softline_apply_warnings(queue_demo_client)
add_executable(example_queue_owner "${PROJECT_SOURCE_DIR}/examples/queue_owner.c")
set_target_properties(example_queue_owner PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/examples")
target_link_libraries(example_queue_owner PRIVATE queue_demo_client)
softline_apply_warnings(example_queue_owner)
softline_local_runtime(example_queue_owner)
