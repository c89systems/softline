#!/usr/bin/env bash
set -euo pipefail
root=$1
real_cmake=$(command -v cmake)
mkdir -p "$root/build"
work=$(mktemp -d "$root/build/target-test.XXXXXX")
trap 'rm -rf "$work"' EXIT
cp "$root/Makefile" "$work/"
mkdir -p "$work/scripts" "$work/bin" "$work/build/debug/examples"
cp "$root/scripts/"{require-host-bash.sh,require-workspace.sh,deps.sh,build.sh,native-linux-target.sh,fuzz.sh,test.sh,terminal-test-env.sh} "$work/scripts/"
mkdir -p "$work/cmake"
cp "$root/cmake/"{softline_workspace.cmake,build-cbor-debug.cmake} "$work/cmake/"
cat > "$work/bin/cmake" <<'SH'
#!/bin/sh
case "$1" in -DSOFTLINE_WORKSPACE_ROOT=*|-DBINARY_DIR=*) exec "$REAL_CMAKE" "$@" ;; esac
if [ "$1" = -S ] && [ "$2" = "$PWD/cmake/terminal-tests" ]; then
  mkdir -p "$4"
  printf '%s/build/terminal-runtime\n' "$PWD" > "$4/install-root.txt"
  printf 'cmake terminal tools\n' >> "$CALL_LOG"
  exit 0
fi
printf 'cmake %s\n' "$*" >> "$CALL_LOG"
SH
cat > "$work/scripts/build-local-lua.sh" <<'SH'
#!/bin/sh
printf 'lua %s\n' "$*" >> "$CALL_LOG"
SH
cat > "$work/scripts/cpkt-toolchains.sh" <<'SH'
#!/usr/bin/env bash
printf '%s %s\n' "$1" "$2" >> "$CALL_LOG"
if [[ "$1" == ensure && -n ${TEST_CBOR_TOOLCHAIN_READY:-} ]]; then
  [[ "$2" == x86_64-linux-gnu ]] || exit 2
  [[ ${TEST_CBOR_PROVISION_FAILURE:-0} == 0 ]] || exit 1
  touch "$TEST_CBOR_TOOLCHAIN_READY"
fi
if [[ "$2" == arm64-apple-darwin && ${TEST_OSXCROSS:-0} == 0 ]]; then
  if [[ "$1" == discover ]]; then
    echo 'status=missing'
  else
    echo 'osxcross SDK unavailable' >&2
    exit 1
  fi
elif [[ "$1" == discover ]]; then
  printf 'status=missing\nroot=/test/osxcross\n'
fi
SH
cat > "$work/scripts/cpkt-aflpp.sh" <<'SH'
#!/usr/bin/env bash
printf 'afl %s\n' "$1" >> "$CALL_LOG"
case "$1" in
  ensure) [[ ${TEST_AFL_FAILURE:-0} == 0 ]] && touch build/afl-ready ;;
  env) [[ -f build/afl-ready ]] && printf 'export CPKT_AFLPP_ROOT=%q\n' "$PWD/build/afl" ;;
esac
SH
cat > "$work/bin/timeout" <<'SH'
#!/bin/sh
printf 'smoke %s\n' "$*" >> "$CALL_LOG"
SH
cat > "$work/bin/ctest" <<'SH'
#!/bin/sh
printf 'ctest %s\n' "$*" >> "$CALL_LOG"
SH
cat > "$work/build/debug/examples/example_chat" <<'SH'
#!/bin/sh
printf '%s %s\n' "$SOFTLINE_CHAT_CHAR_MS" "$SOFTLINE_PROMPT_THEME"
SH
cat > "$work/build/debug/examples/softline_terminal_supervisor" <<'SH'
#!/bin/sh
test "$1" = -- || exit 2
shift
printf 'supervised %s\n' "$*" >> "$CALL_LOG"
exec "$@"
SH
chmod +x "$work/bin/cmake" "$work/scripts/build-local-lua.sh" "$work/build/debug/examples/"{example_chat,softline_terminal_supervisor}
chmod +x "$work/scripts/"cpkt-*.sh "$work/bin/"{timeout,ctest}
export CALL_LOG="$work/calls" PATH="$work/bin:$PATH" REAL_CMAKE="$real_cmake"
cd "$work"
: > "$CALL_LOG"
make --no-print-directory deps DEPENDENCY=libmdf
grep -qx 'cmake --preset debug' "$CALL_LOG"
: > "$CALL_LOG"
make --no-print-directory deps DEPENDENCY=lua PRESET=debug
printf 'cmake --preset debug\nlua %s/build/debug\n' "$work" > "$work/expected"
cmp "$CALL_LOG" "$work/expected"
: > "$CALL_LOG"
if make deps DEPENDENCY=bad > "$work/error" 2>&1; then echo 'Invalid dependency accepted' >&2; exit 1; fi
[[ ! -s "$CALL_LOG" ]]
for theme in default riced; do
  if [[ "$theme" == default ]]; then target=run-chat-without-delay; else target=run-chat-without-delay-riced; fi
  output=$(SOFTLINE_CHAT_CHAR_MS=999 SOFTLINE_PROMPT_THEME=plain make --no-print-directory "$target")
  [[ "$output" == "0 $theme" ]]
  grep -qx 'supervised ./build/debug/examples/example_chat' "$CALL_LOG"
done
: > "$CALL_LOG"
make --no-print-directory build-release
printf 'ensure x86_64-linux-gnu\ncmake --preset x86_64-linux-gnu-release\ncmake --build --preset x86_64-linux-gnu-release\n' > "$work/expected"
cmp "$CALL_LOG" "$work/expected"
if [[ "$(uname -s):$(uname -m)" == Linux:x86_64 ]]; then
  : > "$CALL_LOG"
  make --no-print-directory test
  printf 'ensure x86_64-linux-gnu\ncmake terminal tools\nensure x86_64-linux-gnu\ncmake --preset debug\ncmake --build --preset debug\nctest --preset debug --stop-on-failure --output-on-failure --no-tests=error\n' > "$work/expected"
  cmp "$CALL_LOG" "$work/expected"
fi
mkdir -p fuzz/corpus
printf seed > fuzz/corpus/basic
: > "$CALL_LOG"
if TEST_AFL_FAILURE=1 make --no-print-directory fuzz-smoke > "$work/error" 2>&1; then
  echo 'ERROR: fuzz build continued after provisioning failed' >&2; exit 1
fi
[[ $(cat "$CALL_LOG") == 'afl ensure' ]]
: > "$CALL_LOG"
make --no-print-directory fuzz-smoke
printf 'afl ensure\nafl env\ncmake --preset fuzz\ncmake --build --preset fuzz\nsmoke 15 %s/build/afl/bin/afl-showmap -q -o /dev/null -- ./build/fuzz/softline_stdin_fuzz\n' "$work" > "$work/expected"
cmp "$CALL_LOG" "$work/expected"
# The CBOR Make entry point must hold the same lock across both phases.
cat > "$work/CMakePresets.json" <<'JSON'
{"version": 3, "configurePresets": [{"name": "debug", "generator": "Ninja"}]}
JSON
cat > "$work/check-lock.cmake" <<'CMAKE'
file(LOCK "${LOCK}" GUARD PROCESS TIMEOUT 0 RESULT_VARIABLE result)
if(NOT result STREQUAL "Timeout reached")
  message(FATAL_ERROR "CBOR build lock was not held: ${result}")
endif()
CMAKE
cat > "$work/CMakeLists.txt" <<'CMAKE'
cmake_minimum_required(VERSION 3.24)
if(NOT EXISTS "$ENV{TEST_CBOR_TOOLCHAIN_READY}")
  message(FATAL_ERROR "CBOR configure ran before toolchain provisioning")
endif()
project(cbor_build_fixture NONE)
if(NOT SL_ENABLE_CBOR_TCP OR
   NOT SL_CBOR_SDK_PREFIXES STREQUAL "/sdk one;/sdk two")
  message(FATAL_ERROR "CBOR configuration arguments changed")
endif()
set(LOCK "${CMAKE_BINARY_DIR}/.build.lock")
include(check-lock.cmake)
add_custom_target(check ALL COMMAND "${CMAKE_COMMAND}" "-DLOCK=${LOCK}"
  -P "${CMAKE_SOURCE_DIR}/check-lock.cmake" VERBATIM)
CMAKE
: > "$CALL_LOG"
if TEST_CBOR_TOOLCHAIN_READY="$work/build/cbor-toolchain-ready" TEST_CBOR_PROVISION_FAILURE=1 \
    make --no-print-directory build-cbor-debug > "$work/error" 2>&1; then
  echo 'ERROR: CBOR build continued after provisioning failed' >&2; exit 1
fi
[[ $(cat "$CALL_LOG") == 'ensure x86_64-linux-gnu' ]]
[[ ! -e "$work/build/cbor-debug/CMakeCache.txt" ]]
TEST_CBOR_TOOLCHAIN_READY="$work/build/cbor-toolchain-ready" \
  make --no-print-directory build-cbor-debug 'SL_CBOR_SDK_PREFIXES=/sdk one;/sdk two'
TEST_CBOR_TOOLCHAIN_READY="$work/build/cbor-toolchain-ready" \
  make --no-print-directory build-cbor-debug SL_CBOR_SDK_PREFIXES=
# Packaging consumes resolver readiness: optional Darwin skips an unavailable
# SDK, while required Darwin fails before any producer is invoked.
cp "$root/scripts/"{package.sh,release-targets.sh,release_version.sh} "$work/scripts/"
printf '9.8.7\n' > "$work/VERSION"
cat > "$work/scripts/package-sdk.sh" <<'SH'
#!/bin/sh
printf 'package %s\n' "$2" >> "$CALL_LOG"
touch "$3/softline-9.8.7-$2.tar.gz"
SH
chmod +x "$work/scripts/cpkt-toolchains.sh" "$work/scripts/package-sdk.sh"
export SOFTLINE_PACKAGE_TARGETS='x86_64-linux-musl arm64-apple-darwin'
: > "$CALL_LOG"
SOFTLINE_REQUIRE_DARWIN=0 "$work/scripts/package.sh" > "$work/package.log" 2>&1
grep -q 'SKIP: arm64-apple-darwin' "$work/package.log"
printf 'ensure x86_64-linux-musl\ncmake --preset x86_64-linux-musl-release\ncmake --build --preset x86_64-linux-musl-release\npackage x86_64-linux-musl\ndiscover arm64-apple-darwin\n' > "$work/expected"
cmp "$CALL_LOG" "$work/expected"
[[ $(cat "$work/build/package-state/targets") == x86_64-linux-musl ]]
: > "$CALL_LOG"
if SOFTLINE_REQUIRE_DARWIN=1 "$work/scripts/package.sh" > "$work/error" 2>&1; then
  echo 'ERROR: required Darwin accepted missing SDK readiness' >&2; exit 1
fi
grep -q 'osxcross SDK unavailable' "$work/error"
[[ $(cat "$CALL_LOG") == 'ensure arm64-apple-darwin' ]]
: > "$CALL_LOG"
TEST_OSXCROSS=1 SOFTLINE_PACKAGE_TARGETS=arm64-apple-darwin SOFTLINE_REQUIRE_DARWIN=0 "$work/scripts/package.sh" > "$work/package.log" 2>&1
printf 'discover arm64-apple-darwin\nensure arm64-apple-darwin\ncmake --preset arm64-apple-darwin-release\ncmake --build --preset arm64-apple-darwin-release\npackage arm64-apple-darwin\n' > "$work/expected"
cmp "$CALL_LOG" "$work/expected"
: > "$CALL_LOG"
TEST_OSXCROSS=1 SOFTLINE_REQUIRE_DARWIN=1 "$work/scripts/package.sh" > "$work/package.log" 2>&1
[[ $(head -n 1 "$CALL_LOG") == 'ensure arm64-apple-darwin' ]]
grep -qx 'package arm64-apple-darwin' "$CALL_LOG"
[[ $(wc -l < "$work/build/package-state/targets") == 2 ]]
echo 'Build, fuzz and package targets provision their own selected tools.'
