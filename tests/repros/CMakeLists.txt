if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET softline_static)
  add_executable(test_native_regressions native.c)
  target_link_libraries(test_native_regressions PRIVATE softline_static util)
  target_link_options(test_native_regressions PRIVATE -Wl,--wrap=write)
  target_include_directories(test_native_regressions PRIVATE "${PROJECT_SOURCE_DIR}/tests")
  softline_apply_warnings(test_native_regressions)
  softline_local_runtime(test_native_regressions)
  foreach(case
      clipped-handoff-reopen disconnected-stream-close disconnected-idle-relay
      raw-enter utf8-following-key literal-paste-nul history-record-boundary
      history-recall-after-eviction split-crlf
      insert-borrowed-buffer insert-cluster-boundary)
    add_test(NAME softline_repro_${case} COMMAND test_native_regressions
      "${case}" "${PROJECT_BINARY_DIR}")
    set_tests_properties(softline_repro_${case} PROPERTIES
      LABELS "known-regression" TIMEOUT 5)
  endforeach()
  if(SOFTLINE_TEST_TMUX AND TARGET native_output_fixture)
    foreach(case grow-cleared-history relay-after-close zero-width-wrap quote-without-onlcr)
      add_test(NAME softline_repro_${case} COMMAND bash
        "${CMAKE_CURRENT_SOURCE_DIR}/tmux.sh" "${SOFTLINE_TEST_TMUX}"
        "$<TARGET_FILE:native_output_fixture>" "$<TARGET_FILE:test_native_regressions>"
        "${PROJECT_BINARY_DIR}" "${case}")
      set_tests_properties(softline_repro_${case} PROPERTIES
        LABELS "known-regression" TIMEOUT 8)
    endforeach()
  endif()
endif()
