cmake_minimum_required(VERSION 3.24)
set(work "${ROOT}/build/host-tests/api-fixture")
file(REMOVE_RECURSE "${work}")
set(inputs include/softline/softline.h cmake/softline_version.h.in
  cmake/softline.exports lua/softline_lua.c lua/README.md README.md CMakeLists.txt)
foreach(relative IN LISTS inputs)
  get_filename_component(directory "${work}/${relative}" DIRECTORY)
  file(MAKE_DIRECTORY "${directory}")
  file(COPY_FILE "${ROOT}/${relative}" "${work}/${relative}")
endforeach()
function(check name expected)
  execute_process(COMMAND "${CMAKE_COMMAND}" "-DROOT=${work}"
    -P "${ROOT}/tests/lifecycle/public-api.cmake"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  string(REGEX REPLACE "[ \t\r\n]+" " " diagnostic "${error}")
  if(expected STREQUAL "")
    if(NOT result EQUAL 0)
      message(FATAL_ERROR "${name}: valid API rejected: ${output}${error}")
    endif()
  elseif(result EQUAL 0 OR NOT diagnostic MATCHES "${expected}")
    message(FATAL_ERROR "${name}: expected API rejection '${expected}': ${output}${error}")
  endif()
endfunction()
function(mutate name relative before after expected)
  file(READ "${ROOT}/${relative}" original)
  string(REPLACE "${before}" "${after}" modified "${original}")
  if(original STREQUAL modified)
    message(FATAL_ERROR "${name}: fixture did not change its input")
  endif()
  file(WRITE "${work}/${relative}" "${modified}")
  check("${name}" "${expected}")
  file(WRITE "${work}/${relative}" "${original}")
endfunction()
check(original "")
mutate(c_declaration include/softline/softline.h "int sl_output_stream_begin("
  "int sl_fixture_only(" "C public declarations and dynamic export allowlist differ")
mutate(receiver include/softline/softline.h "(*output_stream_begin)"
  "(*fixture_only)" "Receiver lacks public C counterpart: fixture_only")
mutate(c_config include/softline/softline.h "int history_max_len;"
  "int fixture_only;" "C and Lua configuration fields differ")
mutate(lua_config lua/softline_lua.c "lua_getfield(L, index, \"history_max_len\")"
  "lua_getfield(L, index, \"fixture_only\")" "C and Lua configuration fields differ")
mutate(accepted_config lua/softline_lua.c "\"history_max_len\","
  "\"fixture_only\"," "C and Lua configuration fields differ")
mutate(lua_method lua/softline_lua.c "{\"last_error\", softline_lua_last_error}"
  "{\"fixture_only\", softline_lua_last_error}" "Lua method documentation missing: fixture_only")
mutate(missing_lua_method lua/softline_lua.c "{\"last_error\", softline_lua_last_error},"
  "" "C receiver and Lua method registry differ")
mutate(c_constant lua/softline_lua.c "lua_pushinteger(L, SL_KEY_NONE)"
  "lua_pushinteger(L, SL_FIXTURE_ONLY)" "Missing public Lua constant declaration/doc: SL_FIXTURE_ONLY")
mutate(lua_constant lua/softline_lua.c "lua_setfield(L, -2, \"KEY_NONE\")"
  "lua_setfield(L, -2, \"FIXTURE_ONLY\")" "Missing public Lua constant declaration/doc: SL_KEY_NONE")
mutate(abi CMakeLists.txt "set(SOFTLINE_ABI_VERSION \"5\""
  "set(SOFTLINE_ABI_VERSION \"999\"" "ABI documentation and implementation differ")
mutate(config_docs lua/README.md "`history_max_len`" "history_max_len"
  "Lua config documentation missing: history_max_len")
mutate(method_docs lua/README.md "`sl:last_error(" "`last_error("
  "Lua method documentation missing: last_error")
file(REMOVE_RECURSE "${work}")
message(STATUS "API checks reject changed declarations, receiver/config bindings, constants, ABI and documentation")
