#!/usr/bin/env bash
set -euo pipefail
root=$1
work=$(mktemp -d "$root/build/workspace.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
fixture="$work/source with spaces"
mkdir -p "$fixture/scripts" "$fixture/cmake" "$fixture/build" "$fixture/dist" "$fixture/.cache" "$work/borrowed"
cp "$root/scripts/"{require-workspace.sh,clean.sh,require-host-bash.sh,stage_lua_rock_sources.sh,release_version.sh} "$fixture/scripts/"
cp "$root/cmake/softline_workspace.cmake" "$fixture/cmake/"
cp "$root/scripts/"{package-sdk.sh,package-consumer-smoke.sh,release-targets.sh} "$fixture/scripts/"
entrypoints=(build.sh package.sh test.sh terminal-test-env.sh deps.sh fuzz.sh valgrind.sh test_clangd.sh package-verify.sh validate_luarocks.sh verify-artifact-runtime.sh lua-env.sh lua-debug-env.sh terminal-test-runtime.sh relocate-terminal-test-tools.sh)
for script in "${entrypoints[@]}"; do cp "$root/scripts/$script" "$fixture/scripts/"; done
printf keep > "$work/borrowed/sentinel"
export XDG_CACHE_HOME="$work/global"
unset CPKT_TOOLCHAIN_CACHE CPKT_DEPENDENCY_CACHE
check() { cmake "-DSOFTLINE_WORKSPACE_ROOT=$fixture" -P "$fixture/cmake/softline_workspace.cmake"; }
reject() {
  if "$@" > "$work/error" 2>&1; then echo 'Borrowed workspace/cache accepted' >&2; exit 1; fi
  [[ $(cat "$work/borrowed/sentinel") == keep ]]
}
check
# Direct CMake entrypoints share the checkout's generated-state boundary.
mkdir -p "$fixture/cmake/ownership-probe"
cat > "$fixture/cmake/ownership-probe/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.24)
get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${root}/cmake/softline_workspace.cmake")
softline_check_workspace("${root}")
project(ownership_probe LANGUAGES NONE)
file(WRITE "${CMAKE_BINARY_DIR}/configured" "owned\n")
CMAKE
for output in "$work/borrowed/build" "$fixture/source-build"; do
  reject cmake -S "$fixture/cmake/ownership-probe" -B "$output"
  grep -q 'Generated path must stay below this checkout' "$work/error"
  [[ ! -e "$output/configured" ]]
done
for area in build .cache dist; do
  cmake -S "$fixture/cmake/ownership-probe" -B "$fixture/$area" > "$work/configure" 2>&1
  [[ $(cat "$fixture/$area/configured") == owned ]]
done
ln -s "$fixture" "$work/source-alias"
cmake -S "$work/source-alias/cmake/ownership-probe" -B "$work/source-alias/build" > "$work/configure" 2>&1
CPKT_DEPENDENCY_CACHE="$fixture/build/archives" check
CPKT_TOOLCHAIN_CACHE="$fixture/.cache/toolchains" check
CPKT_DEPENDENCY_CACHE="$XDG_CACHE_HOME/cpkt/deps" check
CPKT_TOOLCHAIN_CACHE="$work/borrowed" reject check
CPKT_DEPENDENCY_CACHE="$work/borrowed" reject check
ln -s "$work/borrowed" "$fixture/build/link"
CPKT_DEPENDENCY_CACHE="$fixture/build/link/cache" reject check
reject cmake -S "$fixture/cmake/ownership-probe" -B "$fixture/build/link/configure"
grep -q 'Generated path must stay below this checkout' "$work/error"
[[ ! -e "$work/borrowed/configure/configured" ]]
reject "$fixture/scripts/stage_lua_rock_sources.sh" "$work/borrowed"
reject "$fixture/scripts/stage_lua_rock_sources.sh" "$fixture/build/link/stage"
[[ ! -e "$work/borrowed/stage" ]]
reject "$fixture/scripts/relocate-terminal-test-tools.sh" "$fixture/build/link" --verify
grep -q 'Generated path must stay below this checkout' "$work/error"
reject "$fixture/scripts/package-consumer-smoke.sh" --producer "$work/borrowed"
grep -q 'Generated path must stay below this checkout' "$work/error"
reject "$fixture/scripts/package-consumer-smoke.sh" --producer "$fixture/build/unprepared"
grep -q 'prepare the selected consumer producer first' "$work/error"
SOFTLINE_TERMINAL_TEST_ROOT="$work/borrowed" reject "$fixture/scripts/terminal-test-runtime.sh" true
grep -q 'Generated path must stay below this checkout' "$work/error"
# Provisioning must validate project ownership before entering standalone helpers.
cp "$root/Makefile" "$fixture/"
for helper in cpkt-toolchains.sh cpkt-aflpp.sh; do
  printf '#!/usr/bin/env bash\nprintf called >> "$PROVISION_CALLS"\n' > "$fixture/scripts/$helper"
  chmod +x "$fixture/scripts/$helper"
done
export PROVISION_CALLS="$work/provision-calls"
for target in build-release package test fuzz-smoke; do
  : > "$PROVISION_CALLS"
  CPKT_TOOLCHAIN_CACHE="$work/borrowed" reject make --no-print-directory -C "$fixture" "$target"
  grep -q 'CPKT_TOOLCHAIN_CACHE must use' "$work/error"
  [[ ! -s "$PROVISION_CALLS" ]]
done
for area in build dist .cache; do
  mv "$fixture/$area" "$fixture/saved"
  ln -s "$work/borrowed" "$fixture/$area"
  reject check
  for script in "${entrypoints[@]}"; do
    reject "$fixture/scripts/$script" "$work/borrowed"
    grep -Fq "$area/ must be owned by this checkout" "$work/error"
  done
  rm "$fixture/$area"
  mv "$fixture/saved" "$fixture/$area"
done
# Every direct configuration entrypoint must reject the same borrowed cache
# before invoking its own generators or compiler checks.
for lane in . cmake/host-tests cmake/lua-tests cmake/retained-resize-tests cmake/local-lua cmake/terminal-tests; do
  mkdir -p "$fixture/$lane"
  cp "$root/$lane/CMakeLists.txt" "$fixture/$lane/"
  CPKT_DEPENDENCY_CACHE="$work/borrowed" reject cmake -S "$fixture/$lane" -B "$fixture/build/configure-${lane//\//-}"
  grep -q 'CPKT_DEPENDENCY_CACHE must use' "$work/error"
done
printf '#!/bin/sh\nexit 0\n' > "$work/borrowed/tool"
for lane in lua-tests retained-resize-tests local-lua; do
  case "$lane" in
    lua-tests) selector=SOFTLINE_LUA_EXECUTABLE ;;
    retained-resize-tests) selector=SOFTLINE_RESIZE_FIXTURE ;;
    local-lua) selector=SOFTLINE_LOCAL_LIBRARY_DIR ;;
  esac
  reject cmake -S "$fixture/cmake/$lane" -B "$fixture/build/foreign-$lane" "-D$selector=$work/borrowed/tool"
  grep -q 'Generated path must stay below this checkout' "$work/error"
done
mkdir -p "$fixture/build/foreign-producer"
printf 'CMAKE_HOME_DIRECTORY:INTERNAL=%s\n' "$work/borrowed" > "$fixture/build/foreign-producer/CMakeCache.txt"
cat > "$fixture/build/foreign-producer/cmake_install.cmake" <<'CMAKE'
foreach(file include/softline/softline.h share/doc/softline/LICENSE share/softline/package-metadata.json lib/libsoftline.a)
  get_filename_component(directory "${CMAKE_INSTALL_PREFIX}/${file}" DIRECTORY)
  file(MAKE_DIRECTORY "${directory}")
  file(WRITE "${CMAKE_INSTALL_PREFIX}/${file}" "foreign fixture")
endforeach()
CMAKE
reject "$fixture/scripts/package-sdk.sh" "$fixture/build/foreign-producer" x86_64-linux-gnu "$fixture/dist"
grep -q 'SDK producer must belong to this checkout' "$work/error"
# Full clean removes hidden local state and preserves the shared global cache.
mkdir -p "$XDG_CACHE_HOME/cpkt/deps" "$XDG_CACHE_HOME/cpkt/toolchains"
printf keep > "$XDG_CACHE_HOME/cpkt/deps/sentinel"
printf keep > "$XDG_CACHE_HOME/cpkt/toolchains/sentinel"
for area in build dist .cache; do printf local > "$fixture/$area/.hidden"; done
"$fixture/scripts/clean.sh" dist
[[ ! -e "$fixture/dist" && -e "$fixture/build/.hidden" && -e "$fixture/.cache/.hidden" ]]
"$fixture/scripts/clean.sh"
[[ ! -e "$fixture/build" && ! -e "$fixture/dist" && ! -e "$fixture/.cache" ]]
[[ $(cat "$XDG_CACHE_HOME/cpkt/deps/sentinel") == keep && $(cat "$XDG_CACHE_HOME/cpkt/toolchains/sentinel") == keep ]]
echo 'Owned workspace/cache checks and scoped/full cleanup passed.'
