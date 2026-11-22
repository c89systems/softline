# Embed libvterm without exporting its namespace in either library variant.
set(_sl_vterm "${CMAKE_CURRENT_SOURCE_DIR}/vendor/libvterm")
set(_sl_vterm_namespace "${CMAKE_CURRENT_BINARY_DIR}/softline_vterm_namespace.h")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${_sl_vterm}/include/vterm.h" "${_sl_vterm}/src/vterm_internal.h")
file(READ "${_sl_vterm}/include/vterm.h" _sl_vterm_header)
file(READ "${_sl_vterm}/src/vterm_internal.h" _sl_vterm_internal)
string(REGEX MATCHALL "vterm_[a-zA-Z0-9_]+[ \t]*\\(" _sl_vterm_symbols
  "${_sl_vterm_header}\n${_sl_vterm_internal}")
list(TRANSFORM _sl_vterm_symbols REPLACE "[ \t]*\\($" "")
list(REMOVE_DUPLICATES _sl_vterm_symbols)
set(_sl_vterm_macros "")
foreach(_symbol IN LISTS _sl_vterm_symbols)
  string(APPEND _sl_vterm_macros "#define ${_symbol} sl_private_${_symbol}\n")
endforeach()
file(GENERATE OUTPUT "${_sl_vterm_namespace}" CONTENT "${_sl_vterm_macros}")

add_library(sl_vterm_obj OBJECT
  vendor/libvterm/src/encoding.c vendor/libvterm/src/keyboard.c
  vendor/libvterm/src/mouse.c vendor/libvterm/src/parser.c
  vendor/libvterm/src/pen.c vendor/libvterm/src/screen.c
  vendor/libvterm/src/state.c vendor/libvterm/src/unicode.c
  vendor/libvterm/src/vterm.c)
set_target_properties(sl_vterm_obj PROPERTIES C_STANDARD 99
  C_VISIBILITY_PRESET hidden POSITION_INDEPENDENT_CODE ON)
target_include_directories(sl_vterm_obj PRIVATE "${_sl_vterm}/include")
target_compile_options(sl_vterm_obj PRIVATE -include "${_sl_vterm_namespace}")

add_library(sl_transcript_obj OBJECT src/softline_transcript.c)
set_target_properties(sl_transcript_obj PROPERTIES POSITION_INDEPENDENT_CODE ON)
softline_apply_warnings(sl_transcript_obj)
target_compile_options(sl_transcript_obj PRIVATE -std=c99 -include "${_sl_vterm_namespace}")
target_include_directories(sl_transcript_obj PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(sl_transcript_obj SYSTEM PRIVATE "${_sl_vterm}/include")

set(SL_PRIVATE_TRANSCRIPT_OBJECTS $<TARGET_OBJECTS:sl_vterm_obj>
  $<TARGET_OBJECTS:sl_transcript_obj>)

string(JSON _dependency_index LENGTH "${SL_BUNDLED_DEPENDENCIES}")
string(JSON SL_BUNDLED_DEPENDENCIES SET "${SL_BUNDLED_DEPENDENCIES}" "${_dependency_index}"
  "{\"name\":\"libvterm\",\"version\":\"0.3.3\",\"license\":\"MIT\",\"provenance\":\"share/softline/licenses/libvterm/provenance.json\"}")
if(SL_INSTALL)
  install(FILES "${_sl_vterm}/LICENSE" "${_sl_vterm}/README.softline.md"
    "${_sl_vterm}/provenance.json"
    DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/softline/licenses/libvterm")
endif()
