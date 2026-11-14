cmake_minimum_required(VERSION 3.24)
include(cmake/softline_workspace.cmake)
softline_check_workspace("${CMAKE_CURRENT_SOURCE_DIR}")
include(cmake/softline_version.cmake)
project(softline VERSION "${PROJECT_VERSION}" LANGUAGES C)

include(GNUInstallDirs)
include(cmake/softline_verified_archive.cmake)
softline_configure_dependency_cache()

option(SL_BUILD_STATIC "Build softline as a static library" ON)
option(SL_BUILD_SHARED "Build softline as a shared library" ON)
option(SL_BUILD_EXAMPLES "Build softline examples" ON)
option(SL_BUILD_TESTS "Build softline tests" ON)
option(SL_INSTALL "Generate install rules" ON)
option(SL_ENABLE_COVERAGE "Enable code coverage" OFF)
option(SL_BUILD_FUZZERS "Build native AFL++ fuzz targets" OFF)
option(SL_WARNINGS_AS_ERRORS "Treat project warnings as errors" ON)
set(SL_TEST_TIMEOUT_SCALE "1" CACHE STRING "Timeout multiplier for instrumented test configurations")
if(NOT SL_TEST_TIMEOUT_SCALE MATCHES "^[1-9][0-9]*$")
  message(FATAL_ERROR "SL_TEST_TIMEOUT_SCALE must be a positive integer")
endif()

if(NOT SL_BUILD_STATIC AND NOT SL_BUILD_SHARED)
  message(FATAL_ERROR "At least one of SL_BUILD_STATIC or SL_BUILD_SHARED must be ON")
endif()

set(SL_DIST_DIR "${CMAKE_SOURCE_DIR}/dist" CACHE PATH "Release artifact output directory")
set(SL_TARGET_ID "" CACHE STRING "Release target identifier")
# The shared-library ABI belongs to the source, not to build configuration.
# Discard cache entries left by older configurations before selecting it.
unset(SOFTLINE_ABI_VERSION CACHE)
set(SOFTLINE_ABI_VERSION "5")

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

function(softline_toolchain_value description key output)
  string(REGEX MATCH "(^|\n)${key}=([^\r\n]+)" _softline_match
    "${description}")
  if(NOT _softline_match)
    message(FATAL_ERROR "Lifecycle resolver did not report ${key}")
  endif()
  set(${output} "${CMAKE_MATCH_2}" PARENT_SCOPE)
endfunction()

function(softline_reject_stale_linux_toolchain_cache)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    return()
  endif()

  set(_softline_expected_target "${SL_TARGET_ID}")
  set(_softline_resolver "${CMAKE_SOURCE_DIR}/scripts/cpkt-toolchains.sh")
  if(NOT _softline_expected_target)
    execute_process(
      COMMAND "${CMAKE_SOURCE_DIR}/scripts/native-linux-target.sh"
      RESULT_VARIABLE _softline_native_result
      OUTPUT_VARIABLE _softline_expected_target
      ERROR_VARIABLE _softline_native_error
      OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _softline_native_result EQUAL 0)
      message(FATAL_ERROR
        "No supported native Bootlin Linux target is available for this build: "
        "${_softline_native_error}")
    endif()
  endif()

  if(NOT _softline_expected_target MATCHES "-linux-")
    return()
  endif()

  execute_process(
    COMMAND "${_softline_resolver}" discover "${_softline_expected_target}"
    RESULT_VARIABLE _softline_discover_result
    OUTPUT_VARIABLE _softline_description
    ERROR_VARIABLE _softline_discover_error)
  if(NOT _softline_discover_result EQUAL 0)
    message(FATAL_ERROR
      "Unable to inspect lifecycle toolchain for ${_softline_expected_target}: "
      "${_softline_discover_error}")
  endif()

  softline_toolchain_value("${_softline_description}" cc _softline_expected_cc)
  softline_toolchain_value("${_softline_description}" ld _softline_expected_ld)

  if(NOT SL_BUILD_FUZZERS AND
     NOT CMAKE_C_COMPILER STREQUAL _softline_expected_cc)
    message(FATAL_ERROR
      "Stale or incompatible CMake cache: ${CMAKE_BINARY_DIR} uses "
      "${CMAKE_C_COMPILER}, but ${_softline_expected_target} requires "
      "${_softline_expected_cc}. Remove this build directory and reconfigure "
      "with the lifecycle preset.")
  endif()

  if(CMAKE_LINKER AND NOT CMAKE_LINKER STREQUAL _softline_expected_ld)
    message(FATAL_ERROR
      "Stale or incompatible CMake cache: ${CMAKE_BINARY_DIR} uses linker "
      "${CMAKE_LINKER}, but ${_softline_expected_target} requires "
      "${_softline_expected_ld}. Remove this build directory and reconfigure "
      "with the lifecycle preset.")
  endif()
endfunction()

softline_reject_stale_linux_toolchain_cache()

function(softline_apply_warnings target)
  if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    target_compile_options(${target}
      PRIVATE
        -std=c89
        -Wall
        -Wextra
        -Wpedantic
        -pedantic-errors
        -Wno-unused-parameter
    )
    if(SL_WARNINGS_AS_ERRORS)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
  if(UNIX)
    target_compile_definitions(${target} PRIVATE _POSIX_C_SOURCE=200809L)
  endif()
endfunction()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND
   CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  add_link_options(-Wl,--fatal-warnings)
endif()

add_library(sl_lib_obj OBJECT src/softline.c src/softline_surface.c
  src/softline_clipboard.c src/softline_history.c src/softline_edit.c
  src/softline_queue_backend.c src/softline_terminal.c src/softline_pty.c
  src/softline_completion.c src/softline_image.c)
set_target_properties(sl_lib_obj PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(sl_lib_obj
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
softline_apply_warnings(sl_lib_obj)
include(cmake/softline_resolver.cmake)

if(SL_BUILD_STATIC)
  add_library(softline_static STATIC)
  target_sources(softline_static PRIVATE $<TARGET_OBJECTS:sl_lib_obj>
    ${SL_PRIVATE_RESOLVER_OBJECTS})
  target_include_directories(softline_static
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  )
  set_target_properties(softline_static PROPERTIES OUTPUT_NAME softline)
  if(SL_BUILD_SHARED)
    set_target_properties(softline_static PROPERTIES EXPORT_NAME softline_static)
  else()
    set_target_properties(softline_static PROPERTIES EXPORT_NAME softline)
  endif()
  if(NOT SL_BUILD_SHARED)
    add_library(softline ALIAS softline_static)
  endif()
endif()

if(SL_BUILD_SHARED)
  set(_softline_export_allowlist "${CMAKE_CURRENT_SOURCE_DIR}/cmake/softline.exports")
  if(NOT EXISTS "${_softline_export_allowlist}")
    message(FATAL_ERROR "Missing shared-library export allowlist: ${_softline_export_allowlist}")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_softline_export_allowlist}")
  file(STRINGS "${_softline_export_allowlist}" _softline_export_symbols)
  if(NOT _softline_export_symbols)
    message(FATAL_ERROR "Shared-library export allowlist is empty")
  endif()
  set(_softline_export_map "${CMAKE_CURRENT_BINARY_DIR}/softline.exports.map")
  set(_softline_darwin_exports "${CMAKE_CURRENT_BINARY_DIR}/softline.exports.darwin")
  set(_softline_export_map_content "SOFTLINE_1 {\n  global:\n")
  set(_softline_darwin_export_content "")
  foreach(_softline_export_symbol IN LISTS _softline_export_symbols)
    string(STRIP "${_softline_export_symbol}" _softline_export_symbol)
    if(_softline_export_symbol STREQUAL "" OR _softline_export_symbol MATCHES "^#")
      continue()
    endif()
    if(NOT _softline_export_symbol MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
      message(FATAL_ERROR "Invalid shared-library export: ${_softline_export_symbol}")
    endif()
    string(APPEND _softline_export_map_content "    ${_softline_export_symbol};\n")
    string(APPEND _softline_darwin_export_content "_${_softline_export_symbol}\n")
  endforeach()
  string(APPEND _softline_export_map_content "  local: *;\n};\n")
  file(GENERATE OUTPUT "${_softline_export_map}"
    CONTENT "${_softline_export_map_content}")
  file(GENERATE OUTPUT "${_softline_darwin_exports}"
    CONTENT "${_softline_darwin_export_content}")

  add_library(softline_shared SHARED)
  target_sources(softline_shared PRIVATE $<TARGET_OBJECTS:sl_lib_obj>
    ${SL_PRIVATE_RESOLVER_OBJECTS})
  target_include_directories(softline_shared
    PUBLIC
      $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
      $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
  )
  set_target_properties(softline_shared PROPERTIES
    EXPORT_NAME softline
    OUTPUT_NAME softline
    VERSION ${PROJECT_VERSION}
    SOVERSION ${SOFTLINE_ABI_VERSION}
    MACOSX_RPATH ON
    INSTALL_NAME_DIR "@rpath"
  )
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(softline_shared PRIVATE
      "LINKER:--version-script,${_softline_export_map}")
    set_property(TARGET softline_shared APPEND PROPERTY LINK_DEPENDS
      "${_softline_export_map}")
  elseif(APPLE)
    target_link_options(softline_shared PRIVATE
      "LINKER:-exported_symbols_list,${_softline_darwin_exports}")
    set_property(TARGET softline_shared APPEND PROPERTY LINK_DEPENDS
      "${_softline_darwin_exports}")
  endif()
  add_library(softline ALIAS softline_shared)
endif()

include(cmake/softline_local_runtime.cmake)
include(cmake/softline_test_libmdf.cmake)

if(UNIX AND (SL_BUILD_EXAMPLES OR SL_BUILD_TESTS))
  include(cmake/softline_queue_demo.cmake)
endif()

if(SL_BUILD_EXAMPLES)
  add_subdirectory(examples)
endif()

if(SL_BUILD_TESTS)
  include(CTest)
  add_subdirectory(tests)
endif()

if(SL_BUILD_FUZZERS)
  add_executable(softline_stdin_fuzz fuzz/softline_stdin_fuzz.c)
  target_link_libraries(softline_stdin_fuzz PRIVATE softline)
  softline_apply_warnings(softline_stdin_fuzz)
  softline_local_runtime(softline_stdin_fuzz)
endif()

if(SL_INSTALL)
  include(CMakePackageConfigHelpers)

  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/softline_version.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/softline
  )

  install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

  if(SL_BUILD_STATIC)
    install(TARGETS softline_static
      EXPORT softlineTargets
      ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
  endif()

  if(SL_BUILD_SHARED)
    install(TARGETS softline_shared
      EXPORT softlineTargets
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
  endif()

  if(SL_BUILD_STATIC OR SL_BUILD_SHARED)
    install(EXPORT softlineTargets
      NAMESPACE softline::
      DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/softline
      FILE softlineTargets.cmake
    )

    configure_package_config_file(
      cmake/softlineConfig.cmake.in
      ${CMAKE_CURRENT_BINARY_DIR}/softlineConfig.cmake
      INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/softline
    )

    write_basic_package_version_file(
      ${CMAKE_CURRENT_BINARY_DIR}/softlineConfigVersion.cmake
      VERSION ${PROJECT_VERSION}
      COMPATIBILITY SameMajorVersion
    )

    if(IS_ABSOLUTE "${CMAKE_INSTALL_LIBDIR}")
      set(SOFTLINE_PC_PREFIX "${CMAKE_INSTALL_PREFIX}")
      set(SOFTLINE_PC_LIBDIR "${CMAKE_INSTALL_FULL_LIBDIR}")
    else()
      file(RELATIVE_PATH SOFTLINE_PC_PREFIX_REL
        "/${CMAKE_INSTALL_LIBDIR}/pkgconfig"
        "/"
      )
      set(SOFTLINE_PC_PREFIX "\${pcfiledir}/${SOFTLINE_PC_PREFIX_REL}")
      set(SOFTLINE_PC_LIBDIR "\${prefix}/${CMAKE_INSTALL_LIBDIR}")
    endif()
    if(IS_ABSOLUTE "${CMAKE_INSTALL_INCLUDEDIR}")
      set(SOFTLINE_PC_INCLUDEDIR "${CMAKE_INSTALL_FULL_INCLUDEDIR}")
    else()
      set(SOFTLINE_PC_INCLUDEDIR "\${prefix}/${CMAKE_INSTALL_INCLUDEDIR}")
    endif()

    configure_file(
      cmake/softline.pc.in
      ${CMAKE_CURRENT_BINARY_DIR}/softline.pc
      @ONLY
    )
    configure_file(
      cmake/softline-package-metadata.json.in
      ${CMAKE_CURRENT_BINARY_DIR}/softline-package-metadata.json
      @ONLY
    )

    install(FILES
      ${CMAKE_CURRENT_BINARY_DIR}/softlineConfig.cmake
      ${CMAKE_CURRENT_BINARY_DIR}/softlineConfigVersion.cmake
      DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/softline
    )

    install(FILES
      ${CMAKE_CURRENT_BINARY_DIR}/softline.pc
      DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
    )

    install(FILES LICENSE README.md
      DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/doc/softline
    )

    install(FILES
      ${CMAKE_CURRENT_BINARY_DIR}/softline-package-metadata.json
      DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/softline
      RENAME package-metadata.json
    )
  endif()
endif()
