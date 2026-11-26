#!/usr/bin/env bash
set -euo pipefail
root=$1
real_make=$(command -v make)
work=$(mktemp -d "$root/build/release-sequence.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
fixture="$work/repo"
mkdir -p "$fixture/scripts" "$fixture/build" "$work/bin"
cp "$root/scripts/"{release.sh,lifecycle-version-contract.sh,release_version.sh,release-targets.sh,require-host-bash.sh,require-workspace.sh} "$fixture/scripts/"
mkdir -p "$fixture/cmake"
cp "$root/cmake/softline_workspace.cmake" "$fixture/cmake/"
cp "$root/Makefile" "$fixture/"
cat > "$fixture/scripts/cpkt-toolchains.sh" <<'SH'
#!/usr/bin/env bash
[[ $1 == ensure ]] || exit 2
printf 'ensure:%s\n' "$2" >> "$CALL_LOG"
if [[ $2 == arm64-apple-darwin && ${TEST_MISSING_DARWIN_SDK:-0} == 1 ]]; then
  echo 'osxcross SDK unavailable' >&2
  exit 1
fi
SH
cat > "$fixture/scripts/cpkt-aflpp.sh" <<'SH'
#!/usr/bin/env bash
[[ $1 == ensure ]] || exit 2
printf 'ensure:afl\n' >> "$CALL_LOG"
SH
for helper in package-source package-checksums package-verify release_lua_artifacts package-consumer-smoke; do
  printf '#!/usr/bin/env bash\nprintf '\''%%s:%%s\\n'\'' %q "$*" >> "$CALL_LOG"\n' "$helper" > "$fixture/scripts/$helper.sh"
done
cat > "$fixture/scripts/package.sh" <<'SH'
#!/usr/bin/env bash
printf 'package:%s:%s:%s\n' "$0" "$SOFTLINE_PACKAGE_TARGETS" "$SOFTLINE_REQUIRE_DARWIN" >> "$CALL_LOG"
SH
cat > "$fixture/scripts/extract-release-source.sh" <<'SH'
#!/usr/bin/env bash
mkdir -p "$2/native/scripts"
root=$(cd "${BASH_SOURCE[0]%/*}/.." && pwd)
cp "$root/scripts/package.sh" "$2/native/scripts/"
cp "$root/scripts/package-consumer-smoke.sh" "$2/native/scripts/"
printf 'extract\n' >> "$CALL_LOG"
printf '%s\n' "$2/native"
SH
cat > "$work/bin/make" <<'SH'
#!/usr/bin/env bash
printf 'make:%s\n' "$*" >> "$CALL_LOG"
SH
chmod +x "$fixture/scripts/"* "$work/bin/make"
printf '/build/\n/dist/\n/.cache/\n' > "$fixture/.gitignore"
source "$root/tests/lifecycle/release-fixture.sh"
release_fixture_init "$fixture"
export PATH="$work/bin:$PATH" CALL_LOG="$work/calls"
unset SL_VERSION_OVERRIDE SL_VERSION_ROOT SOFTLINE_PACKAGE_TARGETS SOFTLINE_RELEASE_TARGETS SOFTLINE_DIST_DIR SOFTLINE_FORCE_TESTS SOFTLINE_CHECKSUM_FILE
: > "$CALL_LOG"
printf retained > "$fixture/build/retained"
if "$fixture/scripts/release.sh" preflight > "$work/error" 2>&1; then echo 'Untagged release accepted' >&2; exit 1; fi
[[ -f "$fixture/build/retained" && ! -s "$CALL_LOG" ]]
git -C "$fixture" tag v1.2.3
if "$fixture/scripts/release.sh" preflight > "$work/error" 2>&1; then echo 'Unsigned release accepted' >&2; exit 1; fi
[[ -f "$fixture/build/retained" && ! -s "$CALL_LOG" ]]
git -C "$fixture" tag -d v1.2.3 >/dev/null
release_fixture_sign "$fixture"
git -C "$fixture" tag v1.2.3
git -C "$fixture" checkout -qb feature/not-a-release
if "$fixture/scripts/release.sh" preflight > "$work/error" 2>&1; then echo 'Feature-branch release accepted' >&2; exit 1; fi
[[ -f "$fixture/build/retained" && ! -s "$CALL_LOG" ]]
git -C "$fixture" checkout -q trunk
"$fixture/scripts/release.sh" preflight
mapfile -t provision < "$CALL_LOG"
[[ ${#provision[@]} == 8 && ${provision[0]} == ensure:arm64-apple-darwin && ${provision[7]} == ensure:afl ]]
: > "$CALL_LOG"
if TEST_MISSING_DARWIN_SDK=1 "$fixture/scripts/release.sh" preflight > "$work/error" 2>&1; then
  echo 'Release preflight accepted an unavailable Darwin SDK' >&2; exit 1
fi
grep -q 'osxcross SDK unavailable' "$work/error"
[[ -f "$fixture/build/retained" && $(cat "$CALL_LOG") == ensure:arm64-apple-darwin ]]
: > "$CALL_LOG"
[[ -f "$fixture/build/retained" && ! -s "$CALL_LOG" ]]
for area in build .cache; do
  for kind in DEPENDENCY TOOLCHAIN; do
    mkdir -p "$fixture/$area/cache"
    if env "CPKT_${kind}_CACHE=$fixture/$area/cache" "$fixture/scripts/release.sh" preflight > "$work/error" 2>&1; then
      echo "Release accepted a disposable $kind cache" >&2; exit 1
    fi
    grep -q 'survive make clean' "$work/error"
    [[ -f "$fixture/build/retained" && ! -s "$CALL_LOG" ]]
  done
done
if env -u CPKT_TOOLCHAIN_CACHE -u CPKT_DEPENDENCY_CACHE XDG_CACHE_HOME="$fixture/build/global-cache" \
    "$fixture/scripts/release.sh" preflight > "$work/error" 2>&1; then
  echo 'Release accepted a disposable default toolchain cache' >&2; exit 1
fi
grep -q 'survive make clean' "$work/error"
[[ -f "$fixture/build/retained" && ! -s "$CALL_LOG" ]]
if SOFTLINE_RELEASE_TARGETS=x86_64-linux-gnu "$fixture/scripts/release.sh" preflight > "$work/error" 2>&1; then
  echo 'Partial final release accepted' >&2; exit 1
fi
[[ ! -s "$CALL_LOG" ]]
"$fixture/scripts/release.sh"
mapfile -t calls < "$CALL_LOG"
[[ ${#calls[@]} == 9 ]]
[[ ${calls[0]} == "make:-C $fixture prerelease-runtime-checks" ]]
[[ ${calls[1]} == package-source: && ${calls[2]} == extract ]]
[[ ${calls[3]} == "package:$fixture/build/release-source/native/scripts/package.sh:x86_64-linux-gnu:0" ]]
[[ ${calls[4]} == "package-consumer-smoke:--producer $fixture/build/release-source/native/build/x86_64-linux-gnu-release" ]]
[[ ${calls[5]} == "package:$fixture/scripts/package.sh:"*":1" ]]
[[ ${calls[5]} != *' x86_64-linux-gnu '* ]]
[[ ${calls[6]} == release_lua_artifacts: && ${calls[7]} == 'package-checksums:all' && ${calls[8]} == package-verify: ]]
[[ -f "$fixture/build/retained" ]]
: > "$CALL_LOG"
"$real_make" --no-print-directory -C "$fixture" release MAKE="$work/bin/make"
mapfile -t calls < "$CALL_LOG"
[[ ${#calls[@]} == 19 && ${calls[0]} == make:lifecycle-version-contract && ${calls[9]} == make:clean ]]
[[ "${calls[*]:1:8}" == "${provision[*]}" ]]
[[ ${calls[10]} == "make:-C $fixture prerelease-runtime-checks" && ${calls[18]} == package-verify: ]]
echo 'Release preflight validates identity, provisions tools and preserves outputs before clean.'
