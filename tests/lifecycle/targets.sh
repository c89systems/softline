#!/usr/bin/env bash
set -euo pipefail
root=$1
real_cmake=$(command -v cmake)
mkdir -p "$root/build"
work=$(mktemp -d "$root/build/target-test.XXXXXX")
trap 'rm -rf "$work"' EXIT
cp "$root/Makefile" "$work/"
mkdir -p "$work/scripts" "$work/bin" "$work/build/debug/examples"
cp "$root/scripts/"{require-host-bash.sh,require-workspace.sh,deps.sh,build.sh} "$work/scripts/"
mkdir -p "$work/cmake"
cp "$root/cmake/softline_workspace.cmake" "$work/cmake/"
cat > "$work/bin/cmake" <<'SH'
#!/bin/sh
case "$1" in -DSOFTLINE_WORKSPACE_ROOT=*) exec "$REAL_CMAKE" "$@" ;; esac
printf 'cmake %s\n' "$*" >> "$CALL_LOG"
SH
cat > "$work/scripts/build-local-lua.sh" <<'SH'
#!/bin/sh
printf 'lua %s\n' "$*" >> "$CALL_LOG"
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
# Packaging consumes resolver readiness: optional Darwin skips an unavailable
# SDK, while required Darwin fails before any producer is invoked.
cp "$root/scripts/"{package.sh,release-targets.sh,release_version.sh} "$work/scripts/"
printf '9.8.7\n' > "$work/VERSION"
cat > "$work/scripts/cpkt-toolchains.sh" <<'SH'
#!/bin/sh
printf 'status=%s\nnote=osxcross SDK unavailable\n' "${TEST_DARWIN_STATUS:-missing}"
SH
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
! grep -q arm64-apple-darwin "$CALL_LOG"
grep -qx 'package x86_64-linux-musl' "$CALL_LOG"
[[ $(cat "$work/build/package-state/targets") == x86_64-linux-musl ]]
: > "$CALL_LOG"
if SOFTLINE_REQUIRE_DARWIN=1 "$work/scripts/package.sh" > "$work/error" 2>&1; then
  echo 'ERROR: required Darwin accepted missing SDK readiness' >&2; exit 1
fi
grep -q 'Darwin artifact is required' "$work/error"
[[ ! -s "$CALL_LOG" ]]
TEST_DARWIN_STATUS=ready SOFTLINE_REQUIRE_DARWIN=1 "$work/scripts/package.sh" > "$work/package.log" 2>&1
grep -qx 'cmake --preset arm64-apple-darwin-release' "$CALL_LOG"
grep -qx 'package arm64-apple-darwin' "$CALL_LOG"
[[ $(wc -l < "$work/build/package-state/targets") == 2 ]]
echo 'Dependency/example targets and optional/required Darwin packaging passed.'
