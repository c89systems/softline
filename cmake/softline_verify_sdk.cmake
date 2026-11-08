cmake_minimum_required(VERSION 3.24)
# Validate identities in the extracted shipment, before consumer compilation.
file(READ "${SDK_ROOT}/share/softline/package-metadata.json" metadata)
foreach(field name version abi_version target_id cmake_package pkg_config)
  string(JSON actual GET "${metadata}" "${field}")
  set(expected softline)
  if(field STREQUAL version)
    set(expected "${SDK_VERSION}")
  elseif(field STREQUAL abi_version)
    set(expected "${SDK_ABI}")
  elseif(field STREQUAL target_id)
    set(expected "${SDK_TARGET}")
  endif()
  if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "SDK metadata ${field}: expected '${expected}', got '${actual}'")
  endif()
endforeach()
file(STRINGS "${SDK_ROOT}/include/softline/softline_version.h" header_version
  REGEX "^#define SOFTLINE_VERSION ")
if(NOT header_version STREQUAL "#define SOFTLINE_VERSION \"${SDK_VERSION}\"")
  message(FATAL_ERROR "SDK header version differs from ${SDK_VERSION}")
endif()

set(dependencies "[]")
if(SDK_TARGET MATCHES "-linux-")
  set(dependencies "[{\"name\":\"c-ares\",\"version\":\"1.34.9\",\"license\":\"MIT AND BSD-3-Clause\",\"provenance\":\"share/softline/licenses/c-ares/provenance.json\"}]")
endif()
string(JSON expected_dependencies GET "${dependencies}")
string(JSON dependency_type TYPE "${metadata}" bundled_dependencies)
string(JSON actual_dependencies GET "${metadata}" bundled_dependencies)
if(NOT dependency_type STREQUAL ARRAY OR NOT actual_dependencies STREQUAL expected_dependencies)
  message(FATAL_ERROR "SDK bundled dependency metadata differs from the declared target closure")
endif()

set(libdir lib)
if(NOT IS_DIRECTORY "${SDK_ROOT}/lib")
  set(libdir lib64)
endif()
set(files include/softline/softline.h include/softline/softline_version.h
  ${libdir}/libsoftline.a ${libdir}/pkgconfig/softline.pc
  ${libdir}/cmake/softline/softlineConfig.cmake
  ${libdir}/cmake/softline/softlineConfigVersion.cmake
  ${libdir}/cmake/softline/softlineTargets.cmake
  ${libdir}/cmake/softline/softlineTargets-release.cmake
  share/doc/softline/LICENSE share/doc/softline/README.md
  share/softline/package-metadata.json)
if(SDK_TARGET MATCHES "-linux-")
  list(APPEND files ${libdir}/libsoftline.so ${libdir}/libsoftline.so.${SDK_ABI}
    ${libdir}/libsoftline.so.${SDK_VERSION})
  foreach(notice AUTHORS LICENSE.md LICENSE.BSD-3-Clause README.softline.md provenance.json softline.patch)
    list(APPEND files "share/softline/licenses/c-ares/${notice}")
  endforeach()
elseif(SDK_TARGET MATCHES "-apple-darwin$")
  list(APPEND files ${libdir}/libsoftline.dylib ${libdir}/libsoftline.${SDK_ABI}.dylib
    ${libdir}/libsoftline.${SDK_VERSION}.dylib)
else()
  message(FATAL_ERROR "Unsupported SDK target: ${SDK_TARGET}")
endif()
# Include only the directory ancestors of declared public files, so empty
# generated/private trees cannot hide from the final payload check either.
set(expected "")
foreach(file IN LISTS files)
  while(NOT file STREQUAL "")
    list(APPEND expected "${file}")
    get_filename_component(file "${file}" DIRECTORY)
  endwhile()
endforeach()
list(REMOVE_DUPLICATES expected)
file(GLOB_RECURSE actual LIST_DIRECTORIES TRUE RELATIVE "${SDK_ROOT}" "${SDK_ROOT}/*")
list(SORT expected)
list(SORT actual)
if(NOT actual STREQUAL expected)
  set(missing "${expected}")
  set(unexpected "${actual}")
  list(REMOVE_ITEM missing ${actual})
  list(REMOVE_ITEM unexpected ${expected})
  message(FATAL_ERROR "SDK payload inventory differs; missing: ${missing}; unexpected: ${unexpected}")
endif()
