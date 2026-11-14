include_guard(GLOBAL)

set(SOFTLINE_TEST_LIBMDF_MODULE_DIR "${CMAKE_CURRENT_LIST_DIR}")

# libmdf is only for examples and integration tests. It must never be linked
# by a Softline library target or appear in exported package metadata.
set(SOFTLINE_TEST_LIBMDF_VERSION "0.23.1")

function(softline_test_libmdf_supported output)
  if(SL_TARGET_ID MATCHES
      "^(x86_64-linux-gnu|x86_64-linux-musl|aarch64-linux-gnu|aarch64-linux-musl|armhf-linux-gnu|armhf-linux-musl|arm64-apple-darwin)$")
    set(${output} TRUE PARENT_SCOPE)
  else()
    set(${output} FALSE PARENT_SCOPE)
  endif()
endfunction()

function(softline_enable_dev_libmdf target)
  softline_test_libmdf_supported(_softline_libmdf_supported)
  if(NOT _softline_libmdf_supported)
    message(FATAL_ERROR
      "libmdf streaming integration requires a supported SL_TARGET_ID; got '${SL_TARGET_ID}'")
  endif()

  set(_softline_libmdf_name
    "libmdf-${SOFTLINE_TEST_LIBMDF_VERSION}-${SL_TARGET_ID}.tar.gz")
  if(SL_TARGET_ID STREQUAL "x86_64-linux-gnu")
    set(_softline_libmdf_sha "5bab790bbe4cafdb3cdcac92fc9238c39acd95902549c6dd28d4df9e6129a3fc")
  elseif(SL_TARGET_ID STREQUAL "x86_64-linux-musl")
    set(_softline_libmdf_sha "fe0d5edb138a48d868d36f20ecc8c5a74b594be633cfdad11289eed64fff01e9")
  elseif(SL_TARGET_ID STREQUAL "aarch64-linux-gnu")
    set(_softline_libmdf_sha "e599314ebf23f90808e17d256331532b7daddf21529e0b58e6b5e57684a2c32c")
  elseif(SL_TARGET_ID STREQUAL "aarch64-linux-musl")
    set(_softline_libmdf_sha "2d17d5b27d40b89679e400ca9ebb25e8c791c3b73a32dcacb094b8e7cae1717b")
  elseif(SL_TARGET_ID STREQUAL "armhf-linux-gnu")
    set(_softline_libmdf_sha "65b48e9b8ce4e6f0485c2a2540b6751a9f7c791adfcf18119b5491dd74fc9cc1")
  elseif(SL_TARGET_ID STREQUAL "armhf-linux-musl")
    set(_softline_libmdf_sha "4dd93d605a287f00287a4b636fdaf09b4dfd4ff86a6464ca069e26c180d00a7c")
  else()
    set(_softline_libmdf_sha "5eca477a9d2cd5523407297fb86a0b900136339098319fad2d18a47c949082a1")
  endif()

  include(${SOFTLINE_TEST_LIBMDF_MODULE_DIR}/softline_verified_archive.cmake)
  softline_verified_archive(
    "libmdf"
    "https://github.com/c89systems/libmdf/releases/download/v${SOFTLINE_TEST_LIBMDF_VERSION}/${_softline_libmdf_name}"
    "${_softline_libmdf_sha}" "${_softline_libmdf_name}" _softline_libmdf_archive)

  set(_softline_libmdf_cache_root
    "${PROJECT_SOURCE_DIR}/.cache")
  set(_softline_libmdf_extract
    "${_softline_libmdf_cache_root}/deps-build/${SL_TARGET_ID}/libmdf")
  set(_softline_libmdf_root
    "${_softline_libmdf_cache_root}/deps/${SL_TARGET_ID}/libmdf/install")
  set(_softline_libmdf_contract
    "${_softline_libmdf_cache_root}/dependency-contracts/${SL_TARGET_ID}/libmdf.txt")
  set(_softline_libmdf_contract_value
    "version=${SOFTLINE_TEST_LIBMDF_VERSION}\narchive_sha256=${_softline_libmdf_sha}\n")
  foreach(path "${_softline_libmdf_extract}" "${_softline_libmdf_root}" "${_softline_libmdf_contract}")
    softline_check_generated_path("${PROJECT_SOURCE_DIR}" "${path}")
  endforeach()
  set(_softline_libmdf_stale FALSE)
  if(NOT EXISTS "${_softline_libmdf_contract}")
    set(_softline_libmdf_stale TRUE)
  else()
    file(READ "${_softline_libmdf_contract}" _softline_libmdf_existing_contract)
    if(NOT _softline_libmdf_existing_contract STREQUAL _softline_libmdf_contract_value)
      set(_softline_libmdf_stale TRUE)
    endif()
  endif()
  if(NOT EXISTS "${_softline_libmdf_root}/lib/pkgconfig/libmdf.pc")
    set(_softline_libmdf_stale TRUE)
  endif()
  if(_softline_libmdf_stale)
    file(REMOVE_RECURSE "${_softline_libmdf_extract}" "${_softline_libmdf_root}")
    file(MAKE_DIRECTORY "${_softline_libmdf_extract}" "${_softline_libmdf_root}")
    file(ARCHIVE_EXTRACT INPUT "${_softline_libmdf_archive}"
      DESTINATION "${_softline_libmdf_extract}")
    set(_softline_libmdf_archive_root
      "${_softline_libmdf_extract}/libmdf-${SOFTLINE_TEST_LIBMDF_VERSION}-${SL_TARGET_ID}")
    if(NOT EXISTS "${_softline_libmdf_archive_root}/lib")
      message(FATAL_ERROR
        "libmdf SDK archive has an unexpected layout: ${_softline_libmdf_archive_root}")
    endif()
    file(COPY "${_softline_libmdf_archive_root}/" DESTINATION "${_softline_libmdf_root}")
    # Release archives normalize timestamps. Mark replaced SDK inputs as new
    # so existing consumers rebuild against the new headers and libraries.
    file(GLOB_RECURSE _softline_libmdf_inputs LIST_DIRECTORIES FALSE
      "${_softline_libmdf_root}/include/*" "${_softline_libmdf_root}/lib/*")
    file(TOUCH ${_softline_libmdf_inputs})
    get_filename_component(_softline_libmdf_contract_dir
      "${_softline_libmdf_contract}" DIRECTORY)
    file(MAKE_DIRECTORY "${_softline_libmdf_contract_dir}")
    file(WRITE "${_softline_libmdf_contract}" "${_softline_libmdf_contract_value}")
  endif()
  if(NOT EXISTS "${_softline_libmdf_root}/lib/pkgconfig/libmdf.pc")
    message(FATAL_ERROR
      "libmdf SDK archive has no pkg-config metadata: ${_softline_libmdf_root}")
  endif()

  find_package(PkgConfig REQUIRED)
  set(_softline_old_pkg_config_path "$ENV{PKG_CONFIG_PATH}")
  set(_softline_old_pkg_config_libdir "$ENV{PKG_CONFIG_LIBDIR}")
  set(_softline_old_pkg_config_sysroot_dir "$ENV{PKG_CONFIG_SYSROOT_DIR}")
  set(ENV{PKG_CONFIG_PATH}
    "${_softline_libmdf_root}/lib/pkgconfig:${_softline_old_pkg_config_path}")
  set(ENV{PKG_CONFIG_LIBDIR} "${_softline_libmdf_root}/lib/pkgconfig")
  # The extracted SDK records absolute target paths. An active cross sysroot
  # would incorrectly prefix those paths during this private lookup.
  set(ENV{PKG_CONFIG_SYSROOT_DIR} "")
  # Resolve the extracted target SDK directly rather than searching for it
  # inside the toolchain sysroot. Absolute library inputs give Ninja link deps.
  set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY NEVER)
  pkg_check_modules(SOFTLINE_TEST_LIBMDF REQUIRED IMPORTED_TARGET
    "libmdf=${SOFTLINE_TEST_LIBMDF_VERSION}")
  set(ENV{PKG_CONFIG_PATH} "${_softline_old_pkg_config_path}")
  set(ENV{PKG_CONFIG_LIBDIR} "${_softline_old_pkg_config_libdir}")
  set(ENV{PKG_CONFIG_SYSROOT_DIR} "${_softline_old_pkg_config_sysroot_dir}")

  target_link_libraries(${target} PRIVATE PkgConfig::SOFTLINE_TEST_LIBMDF)
  set_property(TARGET ${target} APPEND PROPERTY BUILD_RPATH
    "${_softline_libmdf_root}/lib")
  set(SOFTLINE_TEST_LIBMDF_RUNTIME_ROOT "${_softline_libmdf_root}" PARENT_SCOPE)
endfunction()
