#!/usr/bin/env bash
set -euo pipefail
root=$1
[[ $(uname -s):$(uname -m) == Linux:x86_64 ]] || { echo 'SKIP: native x86_64 ELF fixture'; exit 0; }
mkdir -p "$root/build"
work=$(mktemp -d "$root/build/artifact-fixture.XXXXXX")
trap 'rm -rf "$work"' EXIT
resolved=$("$root/scripts/cpkt-toolchains.sh" env x86_64-linux-gnu)
eval "$resolved"
mkdir -p "$work/stage/sdk/bin" "$work/nested" "$work/dist"
printf 'int main(void) { return 0; }\n' > "$work/main.c"
member=sdk/bin/no-extension
build() {
  "$CC" -std=c89 -Wall -Wextra -Wpedantic -pedantic-errors -Werror -Wl,--fatal-warnings \
    "$work/main.c" -o "$work/stage/$member" "$@"
}
verify() {
  local expected=${1:-}
  tar -cJf "$work/nested/payload.tar.xz" -C "$work/stage" "$member"
  (cd "$work/nested"; zip -q "$work/dist/fixture.src.rock" payload.tar.xz)
  if SOFTLINE_DIST_DIR="$work/dist" "$root/scripts/verify-release-privacy.sh" > "$work/log" 2>&1; then
    [[ -z "$expected" ]] || { cat "$work/log"; echo "ERROR: accepted $expected" >&2; exit 1; }
  else
    [[ -n "$expected" ]] || { cat "$work/log"; exit 1; }
    grep -q "$expected" "$work/log"
    grep -Fq "fixture.src.rock!payload.tar.xz!$member" "$work/log"
  fi
}
build; verify
printf 'const char *private_path = "/other-builder/roots/%s/bin/cc";\nint main(void) { return 0; }\n' x86-64--glibc--stable-2026.08-1 > "$work/main.c"
build; verify 'pinned Bootlin collection path'
printf 'int main(void) { return 0; }\n' > "$work/main.c"
build -static -nostdlib -Wl,-e,main; verify
for tag in --enable-new-dtags --disable-new-dtags; do
  for path in '$ORIGIN' '$ORIGIN/../lib' '$ORIGIN:$ORIGIN/lib'; do
    build "-Wl,$tag,-rpath,$path"; verify
  done
  for path in relative . /opt/private/lib '$ORIGIN:' ':$ORIGIN' '$ORIGINbad'; do
    build "-Wl,$tag,-rpath,$path"; verify 'non-relocatable ELF runtime path'
  done
done
build -Wl,--dynamic-linker,/opt/private/ld.so; verify 'non-system ELF interpreter'
build "-Wl,--dynamic-linker,$CPKT_SYSROOT/lib/ld-linux-x86-64.so.2"; verify 'forbidden local/cache path'
mkdir -p "$work/stage/sdk/lib"
member=sdk/lib/libsoftline.so.5
build -shared -fPIC -Wl,-soname,libsoftline.so.5; verify
build -shared -fPIC -Wl,-soname,libsoftline.so.9; verify 'incorrect softline SONAME'
member=sdk/bin/no-extension
build -shared -fPIC -Wl,-soname,/opt/private/libfoo.so; verify 'forbidden ELF dependency/SONAME'
build "-Wl,-rpath,$CPKT_SYSROOT/lib"; verify 'forbidden local/cache path'
printf '\177ELFbroken' > "$work/stage/$member"; verify 'ELF inspection failed'
for path in "$root" "$HOME"; do printf '%s' "$path" > "$work/stage/$member"; verify 'forbidden local/cache path'; done
printf 'ordinary text' > "$work/stage/$member"; verify
printf 'broken archive' > "$work/dist/fixture.src.rock"
if SOFTLINE_DIST_DIR="$work/dist" "$root/scripts/verify-release-privacy.sh" > "$work/log" 2>&1; then
  echo 'ERROR: corrupt source rock accepted' >&2; exit 1
fi
echo 'Nested artifacts enforce public paths, ELF metadata, SONAME and archive integrity.'
