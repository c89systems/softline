# c-ares stays an implementation detail: no public target, headers or link libs.
set(SL_BUNDLED_DEPENDENCIES "[]")
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  return()
endif()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(_sl_cares_source "${CMAKE_CURRENT_SOURCE_DIR}/vendor/c-ares")
set(_sl_cares_binary "${CMAKE_CURRENT_BINARY_DIR}/private-cares")
file(MAKE_DIRECTORY "${_sl_cares_binary}")
set(_sl_cares_namespace "${_sl_cares_binary}/softline_cares_namespace.h")
file(GLOB_RECURSE _sl_cares_namespace_inputs CONFIGURE_DEPENDS
  "${_sl_cares_source}/*.c" "${_sl_cares_source}/*.h")
add_custom_command(OUTPUT "${_sl_cares_namespace}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/cares-namespace.py"
    "${_sl_cares_source}" "${_sl_cares_namespace}"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/scripts/cares-namespace.py"
    "${_sl_cares_source}/provenance.json" ${_sl_cares_namespace_inputs}
  VERBATIM)
add_custom_target(sl_cares_namespace DEPENDS "${_sl_cares_namespace}")
add_dependencies(sl_lib_obj sl_cares_namespace)

function(softline_configure_cares)
  # Each paste owns its channel and drives its sockets; no event threads or
  # global allocator/init changes. Linux needs no WinSock library initialization.
  set(CARES_STATIC ON CACHE BOOL "Private resolver" FORCE)
  set(CARES_SHARED OFF CACHE BOOL "Private resolver" FORCE)
  set(CARES_INSTALL OFF CACHE BOOL "Private resolver" FORCE)
  set(CARES_BUILD_TOOLS OFF CACHE BOOL "Private resolver" FORCE)
  set(CARES_BUILD_TESTS OFF CACHE BOOL "Private resolver" FORCE)
  set(CARES_BUILD_CONTAINER_TESTS OFF CACHE BOOL "Private resolver" FORCE)
  set(CARES_THREADS OFF CACHE BOOL "Private resolver" FORCE)
  set(CARES_STATIC_PIC ON CACHE BOOL "Private resolver" FORCE)
  add_subdirectory("${_sl_cares_source}" "${_sl_cares_binary}" EXCLUDE_FROM_ALL)

  # Reuse upstream's source list, feature checks and configuration. Embed the
  # objects in our archive instead of making consumers link a separate archive.
  get_target_property(_sources c-ares SOURCES)
  list(TRANSFORM _sources PREPEND "${_sl_cares_source}/src/lib/")
  add_library(sl_cares_obj OBJECT ${_sources})
  add_dependencies(sl_cares_obj sl_cares_namespace)
  # Retain upstream's generic RR union accessor. GCC 15 at -O3 warns about
  # other union members despite its key/type checks selecting the 16-byte
  # IPv6 member. Upstream compilation stays outside the project warning gate;
  # our adapter and all consumer links still treat warnings as errors.
  set_target_properties(sl_cares_obj PROPERTIES POSITION_INDEPENDENT_CODE ON
    C_STANDARD 90 C_VISIBILITY_PRESET hidden)
  target_include_directories(sl_cares_obj PRIVATE
    "$<TARGET_PROPERTY:c-ares,INCLUDE_DIRECTORIES>")
  target_compile_definitions(sl_cares_obj PRIVATE HAVE_CONFIG_H=1
    CARES_BUILDING_LIBRARY)
  target_compile_options(sl_cares_obj PRIVATE -include "${_sl_cares_namespace}")
  if(CARES_DEPENDENT_LIBS)
    message(FATAL_ERROR "Private resolver unexpectedly requires: ${CARES_DEPENDENT_LIBS}")
  endif()
endfunction()
softline_configure_cares()

target_sources(sl_lib_obj PRIVATE src/softline_resolver.c)
target_include_directories(sl_lib_obj SYSTEM PRIVATE
  "${_sl_cares_source}/include" "${_sl_cares_binary}")
set(SL_PRIVATE_RESOLVER_OBJECTS "$<TARGET_OBJECTS:sl_cares_obj>")
set(SL_BUNDLED_DEPENDENCIES
  "[{\"name\":\"c-ares\",\"version\":\"1.34.9\",\"license\":\"MIT AND BSD-3-Clause\",\"provenance\":\"share/softline/licenses/c-ares/provenance.json\"}]")

if(SL_INSTALL)
  install(FILES "${_sl_cares_source}/LICENSE.md"
    "${_sl_cares_source}/LICENSE.BSD-3-Clause"
    "${_sl_cares_source}/AUTHORS" "${_sl_cares_source}/provenance.json"
    "${_sl_cares_source}/README.softline.md" "${_sl_cares_source}/softline.patch"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/softline/licenses/c-ares")
endif()
