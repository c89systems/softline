# Shared example/fixture sources. Keep transport tests in hardening presets
# that disable the interactive examples; none of these targets are installed.
add_executable(softline_terminal_supervisor "${PROJECT_SOURCE_DIR}/examples/terminal_supervisor.c")
set_target_properties(softline_terminal_supervisor PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/examples")
softline_apply_warnings(softline_terminal_supervisor)
softline_local_runtime(softline_terminal_supervisor)
add_library(www_mock STATIC "${PROJECT_SOURCE_DIR}/examples/www_mock.c"
  "${PROJECT_SOURCE_DIR}/examples/web_terminal.c")
target_link_libraries(www_mock PUBLIC softline)
softline_apply_warnings(www_mock)
add_library(demo_image_store STATIC "${PROJECT_SOURCE_DIR}/examples/queue_demo_image.c")
target_link_libraries(demo_image_store PUBLIC softline)
softline_apply_warnings(demo_image_store)
add_library(web_chat STATIC "${PROJECT_SOURCE_DIR}/examples/web_chat.c")
target_include_directories(web_chat PUBLIC "${PROJECT_SOURCE_DIR}/examples")
target_link_libraries(web_chat PUBLIC www_mock demo_image_store)
softline_apply_warnings(web_chat)
if(NOT SL_ENABLE_CBOR_TCP)
  return()
endif()
include("${CMAKE_CURRENT_LIST_DIR}/softline_demo_cbor.cmake")
softline_find_demo_cbor()
add_library(queue_demo_wire STATIC "${PROJECT_SOURCE_DIR}/examples/queue_demo_wire.c")
target_compile_definitions(queue_demo_wire PUBLIC SOFTLINE_CBOR_TCP)
target_link_libraries(queue_demo_wire PUBLIC softline C89Cbor::static)
softline_apply_warnings(queue_demo_wire)
add_library(queue_demo_client STATIC "${PROJECT_SOURCE_DIR}/examples/queue_demo_client.c"
  "${PROJECT_SOURCE_DIR}/examples/queue_demo_image_wire.c"
  "${PROJECT_SOURCE_DIR}/examples/queue_demo_timer.c"
  "${PROJECT_SOURCE_DIR}/examples/queue_demo_terminal.c")
target_link_libraries(queue_demo_client PUBLIC queue_demo_wire demo_image_store)
softline_apply_warnings(queue_demo_client)
add_executable(example_queue_owner "${PROJECT_SOURCE_DIR}/examples/queue_owner.c")
set_target_properties(example_queue_owner PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/examples")
target_link_libraries(example_queue_owner PRIVATE queue_demo_client web_chat)
softline_apply_warnings(example_queue_owner)
softline_local_runtime(example_queue_owner)
