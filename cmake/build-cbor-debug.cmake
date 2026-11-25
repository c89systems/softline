cmake_minimum_required(VERSION 3.24)
get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
include("${CMAKE_CURRENT_LIST_DIR}/softline_workspace.cmake")
softline_check_workspace("${root}")
softline_check_generated_path("${root}" "${BINARY_DIR}")

# The owner and remote client may start from separate Make invocations.
# Hold one lock across configure and build so CMake's probe files stay owned.
set(lock "${BINARY_DIR}/.build.lock")
softline_check_generated_path("${root}" "${lock}")
file(LOCK "${lock}" GUARD PROCESS TIMEOUT 600)

set(options -DSL_ENABLE_CBOR_TCP=ON)
if(SL_CBOR_SDK_PREFIXES)
  string(REPLACE ";" "\\;" prefixes "${SL_CBOR_SDK_PREFIXES}")
  list(APPEND options "-DSL_CBOR_SDK_PREFIXES=${prefixes}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" --preset debug -B "${BINARY_DIR}" ${options}
  WORKING_DIRECTORY "${root}" COMMAND_ERROR_IS_FATAL ANY)
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${BINARY_DIR}"
  COMMAND_ERROR_IS_FATAL ANY)
