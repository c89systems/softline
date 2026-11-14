cmake_minimum_required(VERSION 3.24)
get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include(../softline_workspace.cmake)
softline_check_workspace("${root}")
project(softline_lua_tests LANGUAGES NONE)
if(NOT EXISTS "${SOFTLINE_LUA_EXECUTABLE}")
  message(FATAL_ERROR "Prepare the local interpreter and facade with make lua-rock")
endif()
softline_check_generated_path("${root}" "${SOFTLINE_LUA_EXECUTABLE}")
find_program(BASH_EXECUTABLE bash REQUIRED)
get_filename_component(lua_build "${SOFTLINE_LUA_EXECUTABLE}" DIRECTORY)
set(pty_test "${lua_build}/test_console_pty")
enable_testing()
add_test(NAME lua_history COMMAND "${SOFTLINE_LUA_EXECUTABLE}" "${root}/tests/lua_history.lua" "${root}/build")
add_test(NAME lua_runtime COMMAND "${SOFTLINE_LUA_EXECUTABLE}" "${root}/tests/lua_runtime.lua")
file(READ "${lua_build}/runtime-root.txt" runtime_root)
string(STRIP "${runtime_root}" runtime_root)
set(library_root "${root}/build/lua-sdk")
if(SOFTLINE_LUA_DEBUG)
  set(library_root "${root}/build/debug")
endif()
set_tests_properties(lua_runtime PROPERTIES ENVIRONMENT
  "SOFTLINE_TOOLCHAIN_ROOT=${runtime_root};SOFTLINE_EXPECTED_LIBRARY_ROOT=${library_root}")
add_test(NAME lua_smoke COMMAND "${SOFTLINE_LUA_EXECUTABLE}" "${root}/tests/lua_smoke.lua")
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
  set(image_fixtures "${root}/build/debug/tests/test_clipboard_image"
    "${root}/build/debug/tests/clipboard_fixture")
  foreach(name small.png large.png oversized.png sample.jpeg)
    list(APPEND image_fixtures "${root}/build/debug/tests/clipboard-paste-inputs/${name}")
  endforeach()
  foreach(fixture IN LISTS image_fixtures)
    if(NOT EXISTS "${fixture}")
      message(FATAL_ERROR "Missing Lua X11 test prerequisite: ${fixture}. Prepare with make lua-test or make lua-debug-test.")
    endif()
  endforeach()
  add_test(NAME lua_image_paste COMMAND "${SOFTLINE_LUA_EXECUTABLE}" "${root}/tests/lua_image_paste.lua")
  set_tests_properties(lua_image_paste PROPERTIES ENVIRONMENT "DISPLAY=")
  add_test(NAME lua_image_paste_x11 COMMAND "${BASH_EXECUTABLE}" "${root}/scripts/terminal-test-env.sh"
    xvfb-run -a "${root}/build/debug/tests/test_clipboard_image"
    "${root}/build/debug/tests/clipboard_fixture" "${root}/build"
    "${root}/build/debug/tests/clipboard-paste-inputs")
  set_tests_properties(lua_image_paste_x11 PROPERTIES ENVIRONMENT
    "SOFTLINE_TEST_LUA=${SOFTLINE_LUA_EXECUTABLE};SOFTLINE_TEST_LUA_IMAGE_SCRIPT=${root}/tests/lua-fixtures/image-paste.lua"
    REQUIRED_FILES "${image_fixtures}")
endif()
add_test(NAME lua_osc8 COMMAND "${SOFTLINE_LUA_EXECUTABLE}" "${root}/tests/lua_osc8.lua")
add_test(NAME lua_queue_backend COMMAND "${SOFTLINE_LUA_EXECUTABLE}" "${root}/tests/lua_queue_backend.lua")
add_test(NAME lua_backend_coroutines COMMAND "${pty_test}" backend-coroutines "${SOFTLINE_LUA_EXECUTABLE}" "${root}")
foreach(mode history_auto readline simple chat packaged)
  add_test(NAME "lua_console_${mode}" COMMAND "${BASH_EXECUTABLE}"
    "${root}/tests/check_lua_console.sh" "${SOFTLINE_LUA_EXECUTABLE}" "${root}" "${mode}")
endforeach()
add_test(NAME lua_history_editor COMMAND "${pty_test}" history "${SOFTLINE_LUA_EXECUTABLE}" "${root}")
add_test(NAME lua_readline_edit COMMAND "${pty_test}" readline_edit "${SOFTLINE_LUA_EXECUTABLE}" "${root}")
add_test(NAME lua_queue_policy COMMAND "${pty_test}" policy "${SOFTLINE_LUA_EXECUTABLE}" "${root}")
add_test(NAME lua_terminal_handoff COMMAND "${pty_test}" terminal "${SOFTLINE_LUA_EXECUTABLE}" "${root}")
add_test(NAME lua_completion COMMAND "${pty_test}" completion "${SOFTLINE_LUA_EXECUTABLE}" "${root}")
add_test(NAME lua_demo_files COMMAND "${BASH_EXECUTABLE}" "${root}/tests/check_completion_files.sh"
  "${root}" inventory "${SOFTLINE_LUA_EXECUTABLE}" "${root}/tests/lua_demo_files.lua" "${root}")
add_test(NAME lua_simple_completion COMMAND "${BASH_EXECUTABLE}" "${root}/tests/check_completion_files.sh"
  "${root}" interactive "${pty_test}" simple-completion "${SOFTLINE_LUA_EXECUTABLE}" "${root}")
add_test(NAME lua_chat_completion COMMAND "${BASH_EXECUTABLE}" "${root}/tests/check_completion_files.sh"
  "${root}" interactive "${pty_test}" chat-completion "${SOFTLINE_LUA_EXECUTABLE}" "${root}")
set_tests_properties(lua_chat_completion PROPERTIES ENVIRONMENT
  "SOFTLINE_HISTORY_DIR=${root}/build/test-history/lua_chat_completion;SOFTLINE_PROMPT_THEME=plain;SOFTLINE_CHAT_OPERATION_STEP_MS=0")
string(SHA256 chat_history_key "softline.examples.chat")
add_test(NAME lua_chat_policies COMMAND "${lua_build}/test_chat_policies" lua
  "${SOFTLINE_LUA_EXECUTABLE}" "${root}" "${root}/build" "${lua_build}/test_chat_shell"
  "${chat_history_key}.history")
add_test(NAME lua_chat_history COMMAND "${pty_test}" store lua "${SOFTLINE_LUA_EXECUTABLE}"
  "${root}" "${root}/build" "${chat_history_key}.history")
add_test(NAME lua_chat_exit_controls COMMAND "${lua_build}/test_chat_shell" lua-controls
  "${SOFTLINE_LUA_EXECUTABLE}" "${root}" "${root}/build"
  "${chat_history_key}.history" "${lua_build}/test_chat_shell")
add_test(NAME lua_chat_shell COMMAND "${lua_build}/test_chat_shell" lua
  "${SOFTLINE_LUA_EXECUTABLE}" "${root}" "${root}/build"
  "${chat_history_key}.history" "${lua_build}/test_chat_shell")
if(NOT SOFTLINE_LUA_DEBUG)
  foreach(mode mixed_tty watch)
    add_test(NAME "lua_chat_${mode}" COMMAND "${pty_test}" "${mode}" "${SOFTLINE_LUA_EXECUTABLE}" "${root}")
  endforeach()
  add_test(NAME lua_chat_ctrl_c COMMAND "${pty_test}" cancel "${SOFTLINE_LUA_EXECUTABLE}" "${root}")
  add_test(NAME lua_live_output COMMAND "${pty_test}" live "${SOFTLINE_LUA_EXECUTABLE}" "${root}")
  add_test(NAME lua_idle_callback COMMAND "${pty_test}" idle "${SOFTLINE_LUA_EXECUTABLE}" "${root}")
  add_test(NAME lua_queue_approval COMMAND "${pty_test}" approval "${SOFTLINE_LUA_EXECUTABLE}" "${root}")
  add_test(NAME lua_watch_file_gc COMMAND "${pty_test}" watch_gc "${SOFTLINE_LUA_EXECUTABLE}" "${root}")
endif()
get_property(tests DIRECTORY PROPERTY TESTS)
set_tests_properties(${tests} PROPERTIES TIMEOUT 60 LABELS "lua;integration")
