foreach(terminal_root "${SOFTLINE_TERMINAL_TEST_ROOT}" "$ENV{SOFTLINE_TERMINAL_TEST_ROOT}")
  if(NOT terminal_root STREQUAL "")
    softline_check_generated_path("${PROJECT_SOURCE_DIR}" "${terminal_root}")
  endif()
endforeach()
find_package(Threads REQUIRED)
add_executable(test_history test_history.c)
target_link_libraries(test_history PRIVATE softline Threads::Threads)
softline_apply_warnings(test_history)
softline_local_runtime(test_history)
add_test(NAME softline_history COMMAND test_history "${PROJECT_BINARY_DIR}")
set_tests_properties(softline_history PROPERTIES LABELS "unit;history" TIMEOUT 30)

add_executable(test_softline test_softline.c)
target_link_libraries(test_softline PRIVATE softline)
softline_apply_warnings(test_softline)
softline_local_runtime(test_softline)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  if(TARGET example_queue_owner)
    add_executable(test_queue_tcp test_queue_tcp.c)
    target_include_directories(test_queue_tcp PRIVATE "${PROJECT_SOURCE_DIR}/examples")
    target_link_libraries(test_queue_tcp PRIVATE queue_demo_client util)
    softline_apply_warnings(test_queue_tcp)
    softline_local_runtime(test_queue_tcp)
    add_test(NAME softline_queue_tcp COMMAND test_queue_tcp "$<TARGET_FILE:example_queue_owner>")
    set_tests_properties(softline_queue_tcp PROPERTIES LABELS "integration;queue;editing;memcheck" TIMEOUT 15)
  endif()
  add_executable(test_queue_backend test_queue_backend.c)
  target_link_libraries(test_queue_backend PRIVATE softline util)
  softline_apply_warnings(test_queue_backend)
  softline_local_runtime(test_queue_backend)
  add_test(NAME softline_queue_backend COMMAND test_queue_backend)
  set_tests_properties(softline_queue_backend PROPERTIES
    LABELS "unit;queue;editing;memcheck" TIMEOUT 15)
  add_executable(test_edit test_edit.c "${PROJECT_SOURCE_DIR}/src/softline_edit.c")
  softline_apply_warnings(test_edit)
  softline_local_runtime(test_edit)
  add_test(NAME softline_undo_bounds COMMAND test_edit)
  set_tests_properties(softline_undo_bounds PROPERTIES LABELS "unit;editing;memcheck" TIMEOUT 5)
  add_executable(test_suspend test_suspend.c)
  target_link_libraries(test_suspend PRIVATE softline util)
  softline_apply_warnings(test_suspend)
  softline_local_runtime(test_suspend)
  add_test(NAME softline_suspend COMMAND test_suspend)
  add_test(NAME softline_draft_shortcuts COMMAND test_suspend draft-shortcuts)
  set_tests_properties(softline_draft_shortcuts PROPERTIES LABELS "unit;editing;memcheck" TIMEOUT 15)
  add_test(NAME softline_visual_row_navigation COMMAND test_suspend visual-rows)
  set_tests_properties(softline_visual_row_navigation PROPERTIES LABELS "unit;editing;memcheck" TIMEOUT 15)
  add_test(NAME softline_suspend_mutations COMMAND test_suspend mutations)
  add_test(NAME softline_suspend_transitions COMMAND test_suspend transitions)
  add_test(NAME softline_cursor_undo_groups COMMAND test_suspend cursor-groups)
  add_test(NAME softline_suspend_undo_groups COMMAND test_suspend suspend-groups)
  set_tests_properties(softline_cursor_undo_groups softline_suspend_undo_groups PROPERTIES
    LABELS "unit;editing;memcheck" TIMEOUT 15)
  add_test(NAME softline_producer_columns COMMAND test_suspend columns)
  set_tests_properties(softline_suspend_mutations softline_suspend_transitions softline_producer_columns PROPERTIES
    LABELS "unit;editing;memcheck" TIMEOUT 30)
  set_tests_properties(softline_suspend PROPERTIES LABELS "unit;editing;queue;memcheck" TIMEOUT 15)
  add_test(NAME softline_readline_edit COMMAND test_softline readline-edit)
  set_tests_properties(softline_readline_edit PROPERTIES LABELS "unit;editing" TIMEOUT 10)
  add_executable(test_reconcile test_reconcile.c)
  target_link_libraries(test_reconcile PRIVATE softline util)
  softline_apply_warnings(test_reconcile)
  softline_local_runtime(test_reconcile)
  add_test(NAME softline_reconcile COMMAND test_reconcile)
  set_tests_properties(softline_reconcile PROPERTIES LABELS "unit;streaming" TIMEOUT 10)
  add_executable(test_queue_approval test_queue_approval.c)
  add_executable(test_queue_policy test_queue_policy.c)
  target_link_libraries(test_queue_policy PRIVATE softline util)
  softline_apply_warnings(test_queue_policy)
  softline_local_runtime(test_queue_policy)
  add_test(NAME softline_queue_policy COMMAND test_queue_policy)
  add_test(NAME softline_queue_shortcut_precedence COMMAND test_queue_policy shortcut-precedence)
  set_tests_properties(softline_queue_shortcut_precedence PROPERTIES
    LABELS "unit;queue;editing;memcheck" TIMEOUT 15)
  set_tests_properties(softline_queue_policy PROPERTIES
    LABELS "unit;queue;editing;memcheck" TIMEOUT 30)
  target_link_libraries(test_queue_approval PRIVATE softline util)
  softline_apply_warnings(test_queue_approval)
  softline_local_runtime(test_queue_approval)
  add_test(NAME softline_queue_approval COMMAND test_queue_approval)
  set_tests_properties(softline_queue_approval PROPERTIES
    LABELS "unit;queue" TIMEOUT 10)
  add_executable(test_approval_ui test_approval_ui.c)
  target_link_libraries(test_approval_ui PRIVATE softline util)
  softline_apply_warnings(test_approval_ui)
  softline_local_runtime(test_approval_ui)
  add_test(NAME softline_approval_ui COMMAND test_approval_ui)
  set_tests_properties(softline_approval_ui PROPERTIES LABELS "unit;queue" TIMEOUT 15)
  add_executable(retained_resize_fixture retained_resize_fixture.c)
  target_link_libraries(retained_resize_fixture PRIVATE softline)
  softline_apply_warnings(retained_resize_fixture)
  softline_local_runtime(retained_resize_fixture)
  add_executable(test_stream_transport test_stream_transport.c)
  target_link_libraries(test_stream_transport PRIVATE util)
  softline_apply_warnings(test_stream_transport)
  softline_local_runtime(test_stream_transport)
  find_package(Python3 REQUIRED COMPONENTS Interpreter)
  # Optional source-data regeneration; ordinary builds use the checked-in table.
  add_custom_target(unicode-word-data
    COMMAND "${Python3_EXECUTABLE}"
      "${PROJECT_SOURCE_DIR}/scripts/generate-unicode-word-ranges.py"
      "${PROJECT_BINARY_DIR}/softline_unicode_word.h"
    VERBATIM)
  if(TARGET example_chat)
    add_executable(test_console_pty test_console_pty.c)
    target_link_libraries(test_console_pty PRIVATE util)
    softline_apply_warnings(test_console_pty)
    softline_local_runtime(test_console_pty)
    string(SHA256 chat_history_key "softline.examples.chat")
    add_test(NAME softline_chat_history COMMAND test_console_pty store c
      "$<TARGET_FILE:example_chat>" "${PROJECT_SOURCE_DIR}"
      "${PROJECT_BINARY_DIR}" "${chat_history_key}.history")
    set_tests_properties(softline_chat_history PROPERTIES
      LABELS "integration;history;example-smoke" TIMEOUT 20)
  endif()
  if(TARGET example_chat AND NOT CMAKE_C_FLAGS MATCHES "sanitize")
    add_library(chat_no_sleep_guard SHARED chat_no_sleep_guard.c)
    softline_apply_warnings(chat_no_sleep_guard)
    add_test(NAME softline_chat_no_sleep COMMAND test_stream_transport no_sleep
      "$<TARGET_FILE:example_chat>" "$<TARGET_FILE:chat_no_sleep_guard>")
    set_tests_properties(softline_chat_no_sleep PROPERTIES
      LABELS "integration;streaming;example-smoke" TIMEOUT 12)
  endif()
  add_executable(feed_latency_fixture feed_latency_fixture.c)
  target_link_libraries(feed_latency_fixture PRIVATE softline)
  softline_apply_warnings(feed_latency_fixture)
  softline_local_runtime(feed_latency_fixture)
  add_test(NAME softline_feed_latency COMMAND test_stream_transport feed
    "$<TARGET_FILE:feed_latency_fixture>")
  set_tests_properties(softline_feed_latency PROPERTIES
    LABELS "integration;streaming" TIMEOUT 15)
  add_executable(late_scroll_region_fixture late_scroll_region_fixture.c)
  target_link_libraries(late_scroll_region_fixture PRIVATE softline)
  softline_apply_warnings(late_scroll_region_fixture)
  softline_local_runtime(late_scroll_region_fixture)
  add_test(NAME softline_late_scroll_region COMMAND test_stream_transport late
    "$<TARGET_FILE:late_scroll_region_fixture>")
  set_tests_properties(softline_late_scroll_region PROPERTIES
    LABELS "integration;streaming" TIMEOUT 10)
  # Exercise the private wire protocol without requiring an X server or GTK.
  add_executable(clipboard_protocol_fixture clipboard_protocol_fixture.c
    "${PROJECT_SOURCE_DIR}/src/softline_resolver.c"
    $<TARGET_OBJECTS:sl_cares_obj>)
  add_dependencies(clipboard_protocol_fixture sl_cares_namespace)
  target_include_directories(clipboard_protocol_fixture SYSTEM PRIVATE
    "${PROJECT_SOURCE_DIR}/vendor/c-ares/include" "${PROJECT_BINARY_DIR}/private-cares")
  softline_apply_warnings(clipboard_protocol_fixture)
  softline_local_runtime(clipboard_protocol_fixture)
  set(clipboard_inputs "${CMAKE_CURRENT_BINARY_DIR}/clipboard-inputs")
  add_custom_command(OUTPUT "${clipboard_inputs}/small.png" "${clipboard_inputs}/large.png"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/clipboard_images.py" "${clipboard_inputs}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/clipboard_images.py" VERBATIM)
  add_custom_target(clipboard_inputs DEPENDS "${clipboard_inputs}/small.png" "${clipboard_inputs}/large.png")
  add_executable(test_clipboard_protocol test_clipboard_protocol.c)
  add_dependencies(test_clipboard_protocol clipboard_inputs)
  softline_apply_warnings(test_clipboard_protocol)
  softline_local_runtime(test_clipboard_protocol)
  add_test(NAME softline_clipboard_protocol COMMAND test_clipboard_protocol
    "$<TARGET_FILE:clipboard_protocol_fixture>" "${PROJECT_BINARY_DIR}"
    "${clipboard_inputs}/small.png" "${clipboard_inputs}/large.png")
  set_tests_properties(softline_clipboard_protocol PROPERTIES
    LABELS "integration;clipboard" TIMEOUT 30)
  add_executable(resolver_fixture resolver_fixture.c resolver_collision.c
    "${PROJECT_SOURCE_DIR}/src/softline_resolver.c"
    $<TARGET_OBJECTS:sl_cares_obj>)
  add_dependencies(resolver_fixture sl_cares_namespace)
  target_include_directories(resolver_fixture PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_include_directories(resolver_fixture SYSTEM PRIVATE
    "${PROJECT_SOURCE_DIR}/vendor/c-ares/include" "${PROJECT_BINARY_DIR}/private-cares")
  softline_apply_warnings(resolver_fixture)
  softline_local_runtime(resolver_fixture)
  add_test(NAME softline_resolver COMMAND bash
    "${CMAKE_CURRENT_SOURCE_DIR}/lifecycle/resolver.sh"
    "$<TARGET_FILE:resolver_fixture>" "${PROJECT_BINARY_DIR}")
  set_tests_properties(softline_resolver PROPERTIES LABELS "integration;clipboard" TIMEOUT 15)
  if(TARGET softline_static AND NOT CMAKE_C_FLAGS MATCHES "sanitize")
    add_executable(resolver_fixture_static resolver_fixture.c resolver_collision.c)
    add_dependencies(resolver_fixture_static sl_cares_namespace)
    target_include_directories(resolver_fixture_static PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_include_directories(resolver_fixture_static SYSTEM PRIVATE
      "${PROJECT_SOURCE_DIR}/vendor/c-ares/include" "${PROJECT_BINARY_DIR}/private-cares")
    target_link_libraries(resolver_fixture_static PRIVATE softline_static)
    target_link_options(resolver_fixture_static PRIVATE -static -Wl,--fatal-warnings)
    softline_apply_warnings(resolver_fixture_static)
    add_test(NAME softline_resolver_static COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/lifecycle/resolver.sh"
      "$<TARGET_FILE:resolver_fixture_static>" "${PROJECT_BINARY_DIR}")
    set_tests_properties(softline_resolver_static PROPERTIES
      LABELS "integration;clipboard;static" TIMEOUT 15)
  endif()
  if(TARGET softline_static)
    add_test(NAME softline_resolver_symbols COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/lifecycle/resolver-symbols.sh"
      "${CMAKE_NM}" "$<TARGET_FILE:softline_static>")
    set_tests_properties(softline_resolver_symbols PROPERTIES LABELS "unit;abi" TIMEOUT 10)
  endif()
  target_link_libraries(test_softline PRIVATE util)
  add_test(NAME softline_wrapped_output_prompt COMMAND test_softline wrapped-output-prompt)
  add_test(NAME softline_finite_output_prompt COMMAND test_softline finite-output-prompt)
  set_tests_properties(softline_wrapped_output_prompt softline_finite_output_prompt PROPERTIES
    LABELS "unit;streaming;memcheck" TIMEOUT 10)
endif()

add_test(NAME softline_unit COMMAND test_softline)
set_tests_properties(softline_unit PROPERTIES LABELS "unit" TIMEOUT 30)

# RLIMIT_AS cannot bound ASan's reserved virtual address space.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
   NOT CMAKE_C_FLAGS MATCHES "sanitize=address")
  add_executable(test_quoted_prompt_memory test_quoted_prompt_memory.c)
  target_link_libraries(test_quoted_prompt_memory PRIVATE softline)
  softline_apply_warnings(test_quoted_prompt_memory)
  softline_local_runtime(test_quoted_prompt_memory)
  add_test(NAME quoted_prompt_bounded_memory COMMAND test_quoted_prompt_memory)
  set_tests_properties(quoted_prompt_bounded_memory PROPERTIES LABELS "unit" TIMEOUT 30)
endif()

if(TARGET softline_shared AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_test(NAME softline_exports COMMAND bash
      "${PROJECT_SOURCE_DIR}/scripts/verify-public-exports.sh"
    "${CMAKE_NM}" "$<TARGET_FILE:softline_shared>"
    "${PROJECT_SOURCE_DIR}/cmake/softline.exports")
  set_tests_properties(softline_exports PROPERTIES LABELS "unit;abi" TIMEOUT 10)
  add_test(NAME softline_export_reconfigure COMMAND bash
    "${CMAKE_CURRENT_SOURCE_DIR}/lifecycle/export-reconfigure.sh"
    "${PROJECT_SOURCE_DIR}" "${CMAKE_COMMAND}" "${CMAKE_NM}"
    "${SL_TARGET_ID}" "${CMAKE_TOOLCHAIN_FILE}" "${CMAKE_C_COMPILER}"
    "${CMAKE_LINKER}" "${CMAKE_READELF}" "${SOFTLINE_ABI_VERSION}")
  set_tests_properties(softline_export_reconfigure PROPERTIES
    LABELS "integration;abi" TIMEOUT 90)
endif()

softline_test_libmdf_supported(_softline_test_libmdf_supported)
if(_softline_test_libmdf_supported)
  find_package(Threads REQUIRED)
  add_executable(test_libmdf_stream test_libmdf_stream.c)
  target_link_libraries(test_libmdf_stream PRIVATE softline Threads::Threads)
  softline_enable_dev_libmdf(test_libmdf_stream)
  softline_apply_warnings(test_libmdf_stream)
  softline_local_runtime(test_libmdf_stream)
  add_test(NAME softline_libmdf_stream COMMAND test_libmdf_stream)
  set_tests_properties(softline_libmdf_stream PROPERTIES
    LABELS "integration;streaming;libmdf" TIMEOUT 10)
else()
  message(STATUS
    "Skipping libmdf streaming integration test: unsupported target '${SL_TARGET_ID}'")
endif()

if(CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$" AND
   SL_TARGET_ID STREQUAL "x86_64-linux-gnu")
  add_executable(runtime_probe runtime_probe.c)
  softline_apply_warnings(runtime_probe)
  softline_local_runtime(runtime_probe)
  set(runtime_examples)
  if(TARGET example_simple AND TARGET example_chat)
    list(APPEND runtime_examples $<TARGET_FILE:example_simple> $<TARGET_FILE:example_chat>)
  endif()
  add_test(NAME softline_runtime COMMAND bash
    "${CMAKE_CURRENT_SOURCE_DIR}/lifecycle/runtime.sh" "${CMAKE_SYSROOT}"
    "${CMAKE_READELF}"
    $<TARGET_FILE:runtime_probe> $<TARGET_FILE:test_softline>
    $<TARGET_FILE:test_libmdf_stream> ${runtime_examples})
  set_tests_properties(softline_runtime PROPERTIES
    ENVIRONMENT "SOFTLINE_TEST_LIBMDF_RUNTIME_ROOT=${SOFTLINE_TEST_LIBMDF_RUNTIME_ROOT}"
    TIMEOUT 10 LABELS "runtime")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(clipboard_fixture clipboard_fixture.c)
  target_link_libraries(clipboard_fixture PRIVATE softline)
  softline_apply_warnings(clipboard_fixture)
  softline_local_runtime(clipboard_fixture)
  add_executable(native_output_fixture native_output_fixture.c)
  target_link_libraries(native_output_fixture PRIVATE softline)
  softline_apply_warnings(native_output_fixture)
  softline_local_runtime(native_output_fixture)
  add_executable(test_native_resize test_native_resize.c
    "${PROJECT_SOURCE_DIR}/src/softline_surface.c")
  target_include_directories(test_native_resize PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(test_native_resize PRIVATE util)
  softline_apply_warnings(test_native_resize)
  softline_local_runtime(test_native_resize)
  add_test(NAME softline_native_resize COMMAND test_native_resize)
  set_tests_properties(softline_native_resize PROPERTIES
    LABELS "integration;streaming" TIMEOUT 10)
  add_executable(test_examples test_examples.c)
  target_compile_definitions(test_examples PRIVATE
    SL_TEST_WORK_DIR="${PROJECT_BINARY_DIR}")
  target_link_libraries(test_examples PRIVATE util softline)
  softline_apply_warnings(test_examples)
  softline_local_runtime(test_examples)
  add_test(NAME softline_prompt_frames COMMAND test_examples frames)
  set_tests_properties(softline_prompt_frames PROPERTIES
    LABELS "integration;streaming" TIMEOUT 10)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET example_simple AND TARGET example_chat)
  add_test(NAME softline_examples COMMAND test_examples
    $<TARGET_FILE:example_simple>
    $<TARGET_FILE:example_chat>
  )
  set_tests_properties(softline_examples PROPERTIES LABELS "integration" TIMEOUT 60)
  add_test(NAME softline_example_history_isolation COMMAND bash
    "${CMAKE_CURRENT_SOURCE_DIR}/lifecycle/example-history-isolation.sh"
    "$<TARGET_FILE:test_examples>" "$<TARGET_FILE:example_simple>"
    "$<TARGET_FILE:example_chat>" "${PROJECT_BINARY_DIR}")
  set_tests_properties(softline_example_history_isolation PROPERTIES
    LABELS "integration;history;example-smoke" TIMEOUT 65)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET example_chat)
  add_executable(test_terminal_coordination test_terminal_coordination.c)
  softline_apply_warnings(test_terminal_coordination)
  softline_local_runtime(test_terminal_coordination)
  add_test(NAME softline_terminal_resize_coordination COMMAND test_terminal_coordination)
  set_tests_properties(softline_terminal_resize_coordination PROPERTIES
    LABELS "integration;streaming" TIMEOUT 15)
  add_executable(test_chat_shell test_chat_shell.c)
  target_link_libraries(test_chat_shell PRIVATE util)
  softline_apply_warnings(test_chat_shell)
  softline_local_runtime(test_chat_shell)
  add_test(NAME softline_chat_shell COMMAND test_chat_shell c
    "$<TARGET_FILE:example_chat>" "${PROJECT_SOURCE_DIR}" "${PROJECT_BINARY_DIR}"
    "${chat_history_key}.history" "$<TARGET_FILE:test_chat_shell>")
  set_tests_properties(softline_chat_shell PROPERTIES LABELS "integration" TIMEOUT 60)
  add_executable(test_chat_policies test_chat_policies.c)
  target_link_libraries(test_chat_policies PRIVATE util)
  softline_apply_warnings(test_chat_policies)
  softline_local_runtime(test_chat_policies)
  add_test(NAME softline_chat_policies COMMAND test_chat_policies c
    "$<TARGET_FILE:example_chat>" "${PROJECT_SOURCE_DIR}" "${PROJECT_BINARY_DIR}"
    "$<TARGET_FILE:test_chat_shell>" "${chat_history_key}.history")
  set_tests_properties(softline_chat_policies PROPERTIES
    LABELS "integration;queue;example-smoke" TIMEOUT 15)
  add_executable(test_chat_approval test_chat_approval.c)
  target_link_libraries(test_chat_approval PRIVATE util)
  softline_apply_warnings(test_chat_approval)
  softline_local_runtime(test_chat_approval)
  add_test(NAME softline_chat_approval COMMAND test_chat_approval
    "$<TARGET_FILE:example_chat>" "${PROJECT_SOURCE_DIR}" "${PROJECT_BINARY_DIR}"
    "${chat_history_key}.history")
  set_tests_properties(softline_chat_approval PROPERTIES
    LABELS "integration;queue;example-smoke" TIMEOUT 20)
  # A second real terminal engine catches erased blank-cell extent that tmux's
  # grid drops. Keep these host-only test dependencies out of the library.
  find_program(SOFTLINE_TEST_XVFB_RUN xvfb-run)
  find_library(SOFTLINE_TEST_VTE NAMES vte-2.91 libvte-2.91.so.0
    NO_CMAKE_FIND_ROOT_PATH)
  if(SOFTLINE_TEST_XVFB_RUN AND SOFTLINE_TEST_VTE)
    if(DEFINED ENV{SOFTLINE_TERMINAL_TEST_ROOT} AND
       NOT "$ENV{SOFTLINE_TERMINAL_TEST_ROOT}" STREQUAL "")
      set(terminal_test_environment "SOFTLINE_TERMINAL_TEST_ROOT=$ENV{SOFTLINE_TERMINAL_TEST_ROOT}")
      foreach(variable PATH FONTCONFIG_PATH FONTCONFIG_FILE XKB_CONFIG_ROOT NO_AT_BRIDGE TMPDIR
          CPKT_TOOLCHAIN_CACHE CPKT_DEPENDENCY_CACHE)
        list(APPEND terminal_test_environment "${variable}=$ENV{${variable}}")
      endforeach()
      # Cached tool discovery survives ordinary CMake/Ninja regeneration, so
      # retain its matching runtime as well. Do not depend on make test's
      # temporary parent environment on every configure.
      set(SOFTLINE_TERMINAL_TEST_ROOT "$ENV{SOFTLINE_TERMINAL_TEST_ROOT}"
        CACHE INTERNAL "Prepared terminal test runtime" FORCE)
      set(SOFTLINE_TERMINAL_TEST_ENVIRONMENT "${terminal_test_environment}"
        CACHE INTERNAL "Prepared terminal test environment" FORCE)
    endif()
    foreach(adapter output retained editor chat)
      add_executable(test_vte_${adapter} test_vte_${adapter}.c)
      target_link_libraries(test_vte_${adapter} PRIVATE util ${CMAKE_DL_LIBS})
      softline_apply_warnings(test_vte_${adapter})
      softline_local_runtime(test_vte_${adapter})
      if(SOFTLINE_TERMINAL_TEST_ROOT)
        set_property(TARGET test_vte_${adapter} APPEND PROPERTY BUILD_RPATH
          "${SOFTLINE_TERMINAL_TEST_ROOT}/usr/lib/x86_64-linux-gnu"
          "${SOFTLINE_TERMINAL_TEST_ROOT}/lib/x86_64-linux-gnu")
      endif()
    endforeach()
    set(paste_inputs "${CMAKE_CURRENT_BINARY_DIR}/clipboard-paste-inputs")
    add_custom_command(OUTPUT "${paste_inputs}/small.png" "${paste_inputs}/large.png"
      "${paste_inputs}/oversized.png" "${paste_inputs}/sample.jpeg"
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/clipboard_paste_images.py" "${paste_inputs}"
      DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/clipboard_paste_images.py" VERBATIM)
    add_custom_target(clipboard_paste_inputs DEPENDS "${paste_inputs}/small.png" "${paste_inputs}/large.png"
      "${paste_inputs}/oversized.png" "${paste_inputs}/sample.jpeg")
    add_executable(test_clipboard_image test_clipboard_image.c)
    add_dependencies(test_clipboard_image clipboard_paste_inputs)
    target_link_libraries(test_clipboard_image PRIVATE util ${CMAKE_DL_LIBS})
    softline_apply_warnings(test_clipboard_image)
    softline_local_runtime(test_clipboard_image)
    if(SOFTLINE_TERMINAL_TEST_ROOT)
      set_property(TARGET test_clipboard_image APPEND PROPERTY BUILD_RPATH
        "${SOFTLINE_TERMINAL_TEST_ROOT}/usr/lib/x86_64-linux-gnu"
        "${SOFTLINE_TERMINAL_TEST_ROOT}/lib/x86_64-linux-gnu")
    endif()
    if(SOFTLINE_TERMINAL_TEST_ROOT)
      set(terminal_test_environment "${SOFTLINE_TERMINAL_TEST_ENVIRONMENT}")
    endif()
    add_test(NAME softline_clipboard_image_paste COMMAND "${SOFTLINE_TEST_XVFB_RUN}" -a
      "$<TARGET_FILE:test_clipboard_image>" "$<TARGET_FILE:clipboard_fixture>" "${PROJECT_BINARY_DIR}" "${paste_inputs}")
    set_tests_properties(softline_clipboard_image_paste PROPERTIES
      ENVIRONMENT "${terminal_test_environment}" LABELS "integration;clipboard" TIMEOUT 60)
    add_test(NAME softline_clipboard_image_transport COMMAND "${SOFTLINE_TEST_XVFB_RUN}" -a
      "$<TARGET_FILE:test_clipboard_image>" "$<TARGET_FILE:clipboard_fixture>" "${PROJECT_BINARY_DIR}" "${paste_inputs}" transport)
    set_tests_properties(softline_clipboard_image_transport PROPERTIES
      ENVIRONMENT "${terminal_test_environment}" LABELS "integration;clipboard" TIMEOUT 60)
    add_test(NAME softline_terminal_native_output COMMAND "${SOFTLINE_TEST_XVFB_RUN}" -a
      "$<TARGET_FILE:test_vte_output>" native "$<TARGET_FILE:native_output_fixture>")
    set_tests_properties(softline_terminal_native_output PROPERTIES ENVIRONMENT "${terminal_test_environment}")
    set_tests_properties(softline_terminal_native_output PROPERTIES
      LABELS "integration;streaming" TIMEOUT 120)
    add_test(NAME softline_terminal_tab_expansion COMMAND "${SOFTLINE_TEST_XVFB_RUN}" -a
      "$<TARGET_FILE:test_vte_output>" tabs "$<TARGET_FILE:native_output_fixture>")
    set_tests_properties(softline_terminal_tab_expansion PROPERTIES ENVIRONMENT "${terminal_test_environment}")
    set_tests_properties(softline_terminal_tab_expansion PROPERTIES
      LABELS "integration;streaming" TIMEOUT 30)
    foreach(mode control boundaries)
      add_test(NAME softline_terminal_${mode} COMMAND "${SOFTLINE_TEST_XVFB_RUN}" -a
        "$<TARGET_FILE:test_vte_output>" "${mode}" "$<TARGET_FILE:native_output_fixture>")
      set_tests_properties(softline_terminal_${mode} PROPERTIES
        ENVIRONMENT "${terminal_test_environment}" LABELS "integration;streaming" TIMEOUT 90)
    endforeach()
    foreach(mode stream handoff finite tabs)
      add_test(NAME softline_terminal_retained_${mode} COMMAND "${SOFTLINE_TEST_XVFB_RUN}" -a
        "$<TARGET_FILE:test_vte_retained>" "${mode}" "$<TARGET_FILE:native_output_fixture>" "${PROJECT_BINARY_DIR}")
      set_tests_properties(softline_terminal_retained_${mode} PROPERTIES
        ENVIRONMENT "${terminal_test_environment}" LABELS "integration;streaming" TIMEOUT 120)
    endforeach()
    if(terminal_test_environment)
      add_test(NAME softline_terminal_test_environment COMMAND "${CMAKE_COMMAND}"
        "-DROOT=${PROJECT_SOURCE_DIR}" "-DBUILD=${PROJECT_BINARY_DIR}"
        "-DCTEST=${CMAKE_CTEST_COMMAND}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/lifecycle/terminal-environment.cmake")
      set_tests_properties(softline_terminal_test_environment PROPERTIES
        ENVIRONMENT "${terminal_test_environment}" LABELS "integration;dependency" TIMEOUT 45)
    endif()
    add_test(NAME softline_terminal_opaque_feed COMMAND "${SOFTLINE_TEST_XVFB_RUN}" -a
      "$<TARGET_FILE:test_vte_output>" opaque "$<TARGET_FILE:native_output_fixture>")
    set_tests_properties(softline_terminal_opaque_feed PROPERTIES ENVIRONMENT "${terminal_test_environment}")
    set_tests_properties(softline_terminal_opaque_feed PROPERTIES
      LABELS "integration;streaming" TIMEOUT 30)
    if(TARGET example_simple)
      add_executable(editor_resize_fixture editor_resize_fixture.c)
      target_link_libraries(editor_resize_fixture PRIVATE softline)
      softline_apply_warnings(editor_resize_fixture)
      softline_local_runtime(editor_resize_fixture)
      add_test(NAME softline_terminal_readline_resize COMMAND "${SOFTLINE_TEST_XVFB_RUN}" -a
        "$<TARGET_FILE:test_vte_editor>" "$<TARGET_FILE:editor_resize_fixture>")
      set_tests_properties(softline_terminal_readline_resize PROPERTIES ENVIRONMENT "${terminal_test_environment}")
      set_tests_properties(softline_terminal_readline_resize PROPERTIES
        LABELS "integration" TIMEOUT 40)
      add_test(NAME softline_terminal_prompt_themes COMMAND "${SOFTLINE_TEST_XVFB_RUN}" -a
        "$<TARGET_FILE:test_vte_editor>" "$<TARGET_FILE:editor_resize_fixture>" themes)
      set_tests_properties(softline_terminal_prompt_themes PROPERTIES
        ENVIRONMENT "${terminal_test_environment}" LABELS "integration;editing" TIMEOUT 20)
    endif()
    if(_softline_test_libmdf_supported)
      add_executable(unicode_output_fixture unicode_output_fixture.c)
      target_link_libraries(unicode_output_fixture PRIVATE softline)
      softline_enable_dev_libmdf(unicode_output_fixture)
      softline_apply_warnings(unicode_output_fixture)
      softline_local_runtime(unicode_output_fixture)
      add_test(NAME softline_terminal_unicode_output COMMAND "${SOFTLINE_TEST_XVFB_RUN}" -a
        "$<TARGET_FILE:test_vte_output>" unicode
        "$<TARGET_FILE:unicode_output_fixture>" "${PROJECT_BINARY_DIR}")
      set_tests_properties(softline_terminal_unicode_output PROPERTIES ENVIRONMENT "${terminal_test_environment}")
      set_tests_properties(softline_terminal_unicode_output PROPERTIES
        LABELS "integration;streaming;libmdf" TIMEOUT 120)
    endif()
    add_test(NAME softline_terminal_blank_reflow COMMAND "${SOFTLINE_TEST_XVFB_RUN}" -a
      "$<TARGET_FILE:test_vte_chat>" blank "$<TARGET_FILE:example_chat>")
    set_tests_properties(softline_terminal_blank_reflow PROPERTIES ENVIRONMENT "${terminal_test_environment}")
    set_tests_properties(softline_terminal_blank_reflow PROPERTIES
      LABELS "integration;streaming" TIMEOUT 60)
    add_test(NAME softline_chat_line_keys COMMAND "${SOFTLINE_TEST_XVFB_RUN}" -a
      "$<TARGET_FILE:test_vte_chat>" line-keys "$<TARGET_FILE:example_chat>")
    set_tests_properties(softline_chat_line_keys PROPERTIES
      ENVIRONMENT "${terminal_test_environment}"
      LABELS "integration;example-smoke" TIMEOUT 15)
    foreach(approval_case typing queue edit empty discard idle multiline resize)
      add_test(NAME softline_chat_approval_${approval_case} COMMAND "${SOFTLINE_TEST_XVFB_RUN}" -a
        "$<TARGET_FILE:test_vte_chat>" approval "$<TARGET_FILE:example_chat>" "${approval_case}")
      set_tests_properties(softline_chat_approval_${approval_case} PROPERTIES
        ENVIRONMENT "${terminal_test_environment}"
        LABELS "integration;queue;example-smoke" TIMEOUT 15)
    endforeach()
    foreach(_softline_chat_theme plain riced)
      add_test(NAME softline_chat_width_${_softline_chat_theme} COMMAND "${SOFTLINE_TEST_XVFB_RUN}" -a
        "$<TARGET_FILE:test_vte_chat>" width "$<TARGET_FILE:example_chat>" "${_softline_chat_theme}")
      set_tests_properties(softline_chat_width_${_softline_chat_theme} PROPERTIES ENVIRONMENT "${terminal_test_environment}")
      set_tests_properties(softline_chat_width_${_softline_chat_theme} PROPERTIES
        LABELS "integration;streaming;libmdf" TIMEOUT 40)
    endforeach()
    add_test(NAME softline_terminal_vte_tiny COMMAND "${SOFTLINE_TEST_XVFB_RUN}" -a
      "$<TARGET_FILE:test_vte_chat>" tiny "$<TARGET_FILE:example_chat>")
    set_tests_properties(softline_terminal_vte_tiny PROPERTIES
      ENVIRONMENT "${terminal_test_environment}" LABELS "integration;streaming;resize" TIMEOUT 20)
    add_test(NAME softline_terminal_streaming_resize COMMAND "${SOFTLINE_TEST_XVFB_RUN}" -a
      "$<TARGET_FILE:test_vte_chat>" streaming "$<TARGET_FILE:example_chat>")
    set_tests_properties(softline_terminal_streaming_resize PROPERTIES ENVIRONMENT "${terminal_test_environment}")
    set_tests_properties(softline_terminal_streaming_resize PROPERTIES
      LABELS "integration;streaming" TIMEOUT 60)
  endif()
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET example_chat)
  find_program(SOFTLINE_TEST_TMUX tmux)
  if(SOFTLINE_TEST_TMUX)
    add_test(NAME softline_terminal_retained_fragment_resize COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/lifecycle/retained-fragment-resize.sh"
      "${SOFTLINE_TEST_TMUX}" "$<TARGET_FILE:native_output_fixture>" "${PROJECT_BINARY_DIR}")
    set_tests_properties(softline_terminal_retained_fragment_resize PROPERTIES
      LABELS "integration;streaming;resize" TIMEOUT 20)
    function(retained_resize_case name columns rows mode variant draft)
      add_test(NAME "softline_retained_${name}" COMMAND bash
        "${CMAKE_CURRENT_SOURCE_DIR}/lifecycle/retained-resize.sh"
        "${SOFTLINE_TEST_TMUX}" "$<TARGET_FILE:retained_resize_fixture>"
        "${PROJECT_BINARY_DIR}" "${columns}" "${rows}" "${mode}" "${variant}" "${draft}")
      set_tests_properties("softline_retained_${name}" PROPERTIES
        LABELS "integration;streaming;retained-resize" TIMEOUT 30)
    endfunction()
    retained_resize_case(partial_shrink 60 18 retained-observed partial late)
    retained_resize_case(complete_shrink 60 18 retained-observed complete late)
    retained_resize_case(partial_width 60 30 retained-observed partial late)
    retained_resize_case(partial_height 120 18 retained-observed partial late)
    retained_resize_case(partial_growth 160 40 retained-observed partial late)
    retained_resize_case(width_growth 160 30 retained-observed complete late)
    retained_resize_case(watch_shrink 60 18 watch partial late)
    retained_resize_case(yield_shrink 60 18 yield partial late)
    retained_resize_case(watch_draft 60 18 watch partial early)
    retained_resize_case(yield_draft 60 18 yield complete early)
  endif()
  if(SOFTLINE_TEST_TMUX)
    add_test(NAME softline_terminal_native_output_tmux
      COMMAND bash
      "${CMAKE_CURRENT_SOURCE_DIR}/lifecycle/native-output-tmux.sh"
      "${SOFTLINE_TEST_TMUX}" "$<TARGET_FILE:native_output_fixture>"
      "${PROJECT_BINARY_DIR}")
    set_tests_properties(softline_terminal_native_output_tmux PROPERTIES
      LABELS "integration;streaming" TIMEOUT 30)
    add_executable(terminal_resize_fixture terminal_resize_fixture.c)
    target_link_libraries(terminal_resize_fixture PRIVATE softline)
    softline_enable_dev_libmdf(terminal_resize_fixture)
    softline_apply_warnings(terminal_resize_fixture)
    softline_local_runtime(terminal_resize_fixture)
    add_executable(test_tmux_resize test_tmux_resize.c)
    softline_apply_warnings(test_tmux_resize)
    softline_local_runtime(test_tmux_resize)
    foreach(_softline_grow_state idle running)
      add_test(NAME softline_terminal_single_grow_${_softline_grow_state}
        COMMAND test_tmux_resize "${SOFTLINE_TEST_TMUX}"
        "$<TARGET_FILE:example_chat>" "${PROJECT_BINARY_DIR}"
        "$<TARGET_FILE:terminal_resize_fixture>" --single-grow-${_softline_grow_state})
      set_tests_properties(softline_terminal_single_grow_${_softline_grow_state}
        PROPERTIES LABELS "integration;streaming;resize" TIMEOUT 12)
    endforeach()
    add_test(NAME softline_terminal_streaming_tiny COMMAND test_tmux_resize
      "${SOFTLINE_TEST_TMUX}" "$<TARGET_FILE:example_chat>" "${PROJECT_BINARY_DIR}"
      "$<TARGET_FILE:terminal_resize_fixture>" --streaming-tiny)
    set_tests_properties(softline_terminal_streaming_tiny PROPERTIES
      LABELS "integration;streaming;resize" TIMEOUT 12)
    add_test(NAME softline_terminal_producer_resize COMMAND test_tmux_resize
      "${SOFTLINE_TEST_TMUX}" "$<TARGET_FILE:example_chat>" "${PROJECT_BINARY_DIR}"
      "$<TARGET_FILE:terminal_resize_fixture>" --producer-only)
    set_tests_properties(softline_terminal_producer_resize PROPERTIES
      LABELS "integration;streaming" TIMEOUT 45)
    add_test(NAME softline_terminal_resize COMMAND test_tmux_resize
      "${SOFTLINE_TEST_TMUX}" "$<TARGET_FILE:example_chat>" "${PROJECT_BINARY_DIR}"
      "$<TARGET_FILE:terminal_resize_fixture>")
    add_test(NAME softline_terminal_growth COMMAND test_tmux_resize
      "${SOFTLINE_TEST_TMUX}" "$<TARGET_FILE:example_chat>" "${PROJECT_BINARY_DIR}"
      "$<TARGET_FILE:terminal_resize_fixture>" --grow-both)
    set_tests_properties(softline_terminal_growth PROPERTIES
      LABELS "integration;streaming" TIMEOUT 30)
    add_test(NAME softline_terminal_quoted_resize COMMAND test_tmux_resize
      "${SOFTLINE_TEST_TMUX}" "$<TARGET_FILE:example_chat>" "${PROJECT_BINARY_DIR}"
      "$<TARGET_FILE:terminal_resize_fixture>" --quoted)
    set_tests_properties(softline_terminal_quoted_resize PROPERTIES
      LABELS "integration;streaming" TIMEOUT 30)
    add_test(NAME softline_terminal_static_resize COMMAND test_tmux_resize
      "${SOFTLINE_TEST_TMUX}" "$<TARGET_FILE:example_chat>" "${PROJECT_BINARY_DIR}"
      "$<TARGET_FILE:terminal_resize_fixture>" --static)
    add_test(NAME softline_terminal_height_roundtrip COMMAND test_tmux_resize
      "${SOFTLINE_TEST_TMUX}" "$<TARGET_FILE:example_chat>" "${PROJECT_BINARY_DIR}"
      "$<TARGET_FILE:terminal_resize_fixture>" --height-roundtrip)
    set_tests_properties(softline_terminal_height_roundtrip PROPERTIES
      LABELS "integration;streaming" TIMEOUT 30)
    set_tests_properties(softline_terminal_static_resize PROPERTIES
      LABELS "integration;streaming" TIMEOUT 30)
    set_tests_properties(softline_terminal_resize PROPERTIES
      LABELS "integration;streaming" TIMEOUT 90)
  endif()
endif()

# These C executable cases own native Memcheck coverage. Terminal engine
# adapters keep their ordinary scope rather than instrumenting host tools.
# The RLIMIT_AS budget check measures native memory use, without analysis overhead.
set(_softline_memcheck_tests softline_history softline_unit softline_readline_edit softline_queue_approval softline_approval_ui
  softline_libmdf_stream softline_examples softline_prompt_frames softline_native_resize)
get_property(_softline_tests DIRECTORY PROPERTY TESTS)
foreach(_softline_test IN LISTS _softline_tests)
  get_test_property(${_softline_test} TIMEOUT _softline_timeout)
  if(_softline_timeout AND SL_TEST_TIMEOUT_SCALE GREATER 1)
    math(EXPR _softline_timeout "${_softline_timeout} * ${SL_TEST_TIMEOUT_SCALE}")
    set_tests_properties(${_softline_test} PROPERTIES TIMEOUT ${_softline_timeout})
  endif()
  if(_softline_test IN_LIST _softline_memcheck_tests)
    set_property(TEST ${_softline_test} APPEND PROPERTY LABELS memcheck)
  endif()
  set_property(TEST "${_softline_test}" APPEND PROPERTY ENVIRONMENT
    "SOFTLINE_HISTORY_DIR=${PROJECT_BINARY_DIR}/test-history/${_softline_test}")
endforeach()
