cmake_minimum_required(VERSION 3.24)
set(work "${ROOT}/build/host-tests/sdk-metadata")
file(REMOVE_RECURSE "${work}")
set(metadata "${work}/share/softline/package-metadata.json")
set(header "${work}/include/softline/softline_version.h")
function(prepare target libdir)
  file(REMOVE_RECURSE "${work}")
  set(files include/softline/softline.h include/softline/softline_version.h
    include/softline/terminal.h include/softline/transcript.h include/softline/www.h
    ${libdir}/libsoftline.a ${libdir}/pkgconfig/softline.pc
    ${libdir}/cmake/softline/softlineConfig.cmake
    ${libdir}/cmake/softline/softlineConfigVersion.cmake
    ${libdir}/cmake/softline/softlineTargets.cmake
    ${libdir}/cmake/softline/softlineTargets-release.cmake
    share/doc/softline/LICENSE share/doc/softline/README.md
    share/doc/softline/web-frontend.md share/doc/softline/web-terminal.md
    share/doc/softline/terminal-handoff-design.md share/doc/softline/transcripts.md
    share/doc/softline/streaming-markdown/LICENSE share/doc/softline/streaming-markdown/README.softline
    share/doc/softline/jetbrains-mono/OFL.txt share/doc/softline/jetbrains-mono/README.softline
    share/doc/softline/xterm/LICENSE share/doc/softline/xterm/LICENSE.addon-fit share/doc/softline/xterm/README.softline
    share/softline/licenses/libvterm/LICENSE share/softline/licenses/libvterm/README.softline.md
    share/softline/licenses/libvterm/provenance.json
    share/softline/package-metadata.json)
  set(dependencies "[]")
  if(target MATCHES "-linux-")
    list(APPEND files ${libdir}/libsoftline.so ${libdir}/libsoftline.so.1
      ${libdir}/libsoftline.so.1.2.3)
    foreach(notice AUTHORS LICENSE.md LICENSE.BSD-3-Clause README.softline.md provenance.json softline.patch)
      list(APPEND files "share/softline/licenses/c-ares/${notice}")
    endforeach()
    set(dependencies "[{\"name\":\"c-ares\",\"version\":\"1.34.9\",\"license\":\"MIT AND BSD-3-Clause\",\"provenance\":\"share/softline/licenses/c-ares/provenance.json\"}]")
  else()
    list(APPEND files ${libdir}/libsoftline.dylib ${libdir}/libsoftline.1.dylib
      ${libdir}/libsoftline.1.2.3.dylib)
  endif()
  string(JSON idx LENGTH "${dependencies}")
  string(JSON dependencies SET "${dependencies}" "${idx}"
    "{\"name\":\"libvterm\",\"version\":\"0.3.3\",\"license\":\"MIT\",\"provenance\":\"share/softline/licenses/libvterm/provenance.json\"}")
  foreach(file IN LISTS files)
    file(WRITE "${work}/${file}" "fixture\n")
  endforeach()
  set(good "{\"name\":\"softline\",\"version\":\"1.2.3\",\"abi_version\":\"1\",\"target_id\":\"${target}\",\"cmake_package\":\"softline\",\"pkg_config\":\"softline\",\"bundled_dependencies\":${dependencies}}")
  file(WRITE "${metadata}" "${good}")
  file(WRITE "${header}" "#define SOFTLINE_VERSION \"1.2.3\"\n")
  set(good "${good}" PARENT_SCOPE)
endfunction()
function(check expected)
  execute_process(COMMAND "${CMAKE_COMMAND}" "-DSDK_ROOT=${work}"
    -DSDK_VERSION=1.2.3 -DSDK_ABI=1 "-DSDK_TARGET=${target}"
    -P "${ROOT}/cmake/softline_verify_sdk.cmake"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  if(expected STREQUAL "")
    if(NOT result EQUAL 0)
      message(FATAL_ERROR "Valid SDK metadata rejected: ${output}${error}")
    endif()
  elseif(result EQUAL 0 OR NOT error MATCHES "${expected}")
    message(FATAL_ERROR "Expected SDK rejection '${expected}': ${output}${error}")
  endif()
endfunction()
foreach(target arm64-apple-darwin x86_64-linux-gnu)
  foreach(libdir lib lib64)
    prepare("${target}" "${libdir}")
    check("")
    foreach(field name version abi_version target_id cmake_package pkg_config)
      string(JSON bad SET "${good}" "${field}" "\"wrong\"")
      file(WRITE "${metadata}" "${bad}")
      check("SDK metadata ${field}")
    endforeach()
    string(JSON bad SET "${good}" bundled_dependencies "[{\"name\":\"wrong\"}]")
    file(WRITE "${metadata}" "${bad}")
    check("SDK bundled dependency metadata")
    string(JSON bad SET "${good}" bundled_dependencies "\"[]\"")
    file(WRITE "${metadata}" "${bad}")
    check("SDK bundled dependency metadata")
    string(JSON bad SET "${good}" bundled_dependencies "[]")
    file(WRITE "${metadata}" "${bad}")
    check("SDK bundled dependency metadata")
    string(JSON last LENGTH "${good}" bundled_dependencies)
    math(EXPR last "${last} - 1")
    foreach(index RANGE ${last})
      foreach(field name version license provenance)
        string(JSON bad SET "${good}" bundled_dependencies ${index} "${field}" "\"wrong\"")
        file(WRITE "${metadata}" "${bad}")
        check("SDK bundled dependency metadata")
      endforeach()
    endforeach()
    file(WRITE "${metadata}" "${good}")
    foreach(extra include/softline/private.h build/CMakeCache.txt .cache/deps/input
        lua/softline.so ${libdir}/libextra.a)
      prepare("${target}" "${libdir}")
      file(WRITE "${work}/${extra}" "private fixture\n")
      check("SDK payload inventory")
    endforeach()
    prepare("${target}" "${libdir}")
    file(MAKE_DIRECTORY "${work}/build")
    check("SDK payload inventory")
    file(REMOVE_RECURSE "${work}/build")
    foreach(missing share/doc/softline/README.md ${libdir}/libsoftline.a
        ${libdir}/cmake/softline/softlineTargets-release.cmake)
      file(REMOVE "${work}/${missing}")
      check("SDK payload inventory")
      file(WRITE "${work}/${missing}" "fixture\n")
    endforeach()
    file(WRITE "${header}" "#define SOFTLINE_VERSION \"9.9.9\"\n")
    check("SDK header version")
  endforeach()
endforeach()
file(REMOVE_RECURSE "${work}")
message(STATUS "SDK identity, dependency metadata and exact Linux/Darwin lib/lib64 payload checks passed")
