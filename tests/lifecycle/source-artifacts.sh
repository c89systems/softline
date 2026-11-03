#!/usr/bin/env bash
set -euo pipefail
root=$1
mkdir -p "$root/build"
work=$(mktemp -d "$root/build/source-artifacts.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
fixture="$work/source"
mkdir -p "$fixture"
# An extracted-source context supplies its own manifest, even below a checkout.
files=(LICENSE README.md softline.rockspec.in lua/README.md lua/softline_lua.c
  lua/softline_queue_backend_lua.h
  lua/softline_terminal_lua.h
  cmake/softline_workspace.cmake scripts/require-workspace.sh
  scripts/package-source.sh scripts/extract-release-source.sh scripts/release_lua_artifacts.sh scripts/stage_lua_rock_sources.sh
  scripts/build_lua_rock.sh scripts/require-host-bash.sh scripts/release_version.sh
  scripts/render_release_rockspec.sh scripts/render_lua_rockspec.sh
  include/softline/softline.h tests/lua_smoke.lua tests/lua_readline.lua
  tests/lua_watch_file_gc.lua examples/simple.lua examples/chat.lua examples/completion.lua)
for file in "${files[@]}"; do
  mkdir -p "$fixture/$(dirname "$file")"
  cp -p "$root/$file" "$fixture/$file"
done
printf '9.8.7\n' > "$fixture/VERSION"
printf '%s\n' "${files[@]}" VERSION RELEASE_MANIFEST | LC_ALL=C sort > "$fixture/RELEASE_MANIFEST"
unset SL_VERSION_OVERRIDE SL_VERSION_ROOT SOURCE_URL SOURCE_DIR
export SOFTLINE_DIST_DIR="$fixture/dist"
for run in first second; do
  if [[ $run == first ]]; then
    mask=077
    stamp=200101010101
    zone=Pacific/Honolulu
  else
    mask=002
    stamp=202502020202
    zone=Europe/Stockholm
  fi
  find "$fixture" -path "$fixture/build" -prune -o -path "$fixture/dist" -prune -o -type f -exec touch -t "$stamp" {} +
  (
    umask "$mask"
    export TZ="$zone"
    "$fixture/scripts/package-source.sh"
    "$fixture/scripts/release_lua_artifacts.sh"
  )
  (cd "$fixture/dist"; sha256sum softline-* | LC_ALL=C sort) > "$work/$run"
done
cmp "$work/first" "$work/second"
grep -Fxq '   url = "https://github.com/c89systems/softline/releases/download/v9.8.7/softline-lua-9.8.7.tar.gz",' \
  "$fixture/dist/softline-9.8.7-1.rockspec"
mkdir "$work/extracted" "$work/lua"
tar -xzf "$fixture/dist/softline-9.8.7.tar.gz" -C "$work/extracted"
tar -xzf "$fixture/dist/softline-lua-9.8.7.tar.gz" -C "$work/lua"
for payload in "$work/extracted/softline-9.8.7" "$work/lua/softline-lua-9.8.7"; do
  (cd "$payload"; find . -type f | sed 's|^./||' | LC_ALL=C sort) > "$work/actual"
  LC_ALL=C sort "$payload/RELEASE_MANIFEST" > "$work/expected"
  cmp "$work/actual" "$work/expected"
  [[ $(cat "$payload/VERSION") == 9.8.7 ]]
  [[ $(stat -c %a "$payload/scripts/require-host-bash.sh") == 755 ]]
  [[ $(stat -c %a "$payload/include/softline/softline.h") == 644 ]]
  "$payload/scripts/render_lua_rockspec.sh" "$work/rendered.rockspec" >/dev/null
  grep -Fxq 'version = "9.8.7-1"' "$work/rendered.rockspec"
done
mkdir "$work/rock"
unzip -q "$fixture/dist/softline-9.8.7-1.src.rock" -d "$work/rock"
for member in softline-9.8.7-1.rockspec softline-lua-9.8.7.tar.gz; do
  cmp "$fixture/dist/$member" "$work/rock/$member"
done
[[ $(find "$work/rock" -type f | wc -l) == 2 ]]
extract="$fixture/scripts/extract-release-source.sh"
archive="$fixture/dist/softline-9.8.7.tar.gz"
source=$("$extract" "$archive" "$fixture/build/unpacked")
[[ $source == "$fixture/build/unpacked/softline-9.8.7" ]]
reject() {
  local expected=$1 input=$2 workspace=$3
  if "$extract" "$input" "$workspace" > "$work/rejection" 2>&1; then
    echo "ERROR: source extraction accepted $expected" >&2
    exit 1
  fi
  grep -qF "$expected" "$work/rejection" || { cat "$work/rejection"; exit 1; }
}
reject 'must be empty' "$archive" "$fixture/build/unpacked"
mkdir "$work/outside"
ln -s "$work/outside" "$fixture/build/redirected"
reject 'symlink parents' "$archive" "$fixture/build/redirected/child"
[[ -z $(find "$work/outside" -mindepth 1 -print -quit) ]]
printf corrupt > "$work/corrupt.tar.gz"
reject 'gzip:' "$work/corrupt.tar.gz" "$fixture/build/corrupt"
mkdir -p "$work/unsafe/softline-9.8.7/build"
tar -czf "$work/unsafe.tar.gz" -C "$work/unsafe" softline-9.8.7
reject 'forbidden source archive entry' "$work/unsafe.tar.gz" "$fixture/build/unsafe"
rm -r "$work/unsafe/softline-9.8.7/build"
ln -s /etc/passwd "$work/unsafe/softline-9.8.7/link"
tar -czf "$work/unsafe.tar.gz" -C "$work/unsafe" softline-9.8.7
reject 'non-file/directory entry' "$work/unsafe.tar.gz" "$fixture/build/links"
echo 'Actual source/Lua entrypoints produce deterministic bytes and exact manifests, modes and source-rock closure.'
