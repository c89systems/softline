#!/usr/bin/env bash
set -euo pipefail
root=$1
cmake=$2
nm=$3
target=$4
toolchain=$5
compiler=$6
linker=$7
readelf=$8
abi=$9
stale_abi=$((abi - 1))
mkdir -p "$root/build"
work=$(mktemp -d "$root/build/export-reconfigure.XXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir "$work/source"
cp "$root/"{CMakeLists.txt,LICENSE,README.md} "$work/source/"
cp -r "$root/"{src,include,cmake,scripts,vendor,web,docs} "$work/source/"
mkdir "$work/source/lua"
cp "$root/lua/README.md" "$work/source/lua/"
case "$toolchain" in "$root/"*) toolchain="$work/source/${toolchain#$root/}" ;; esac
cat > "$work/launcher" <<'SH'
#!/bin/sh
printf 'compiler\n' >> "$COMPILER_CALLS"
exec "$@"
SH
chmod +x "$work/launcher"
export COMPILER_CALLS="$work/calls"
build="$work/source/build"
args=(-S "$work/source" -B "$build" -G Ninja "-DSL_TARGET_ID=$target"
  "-DCMAKE_C_COMPILER=$compiler" "-DCMAKE_LINKER=$linker" "-DCMAKE_TOOLCHAIN_FILE=$toolchain"
  -DCMAKE_LINK_DEPENDS_USE_LINKER=FALSE -DSL_BUILD_STATIC=OFF -DSL_BUILD_TESTS=OFF
  -DSL_BUILD_EXAMPLES=OFF -DSL_INSTALL=ON -DCMAKE_INSTALL_LIBDIR=lib
  "-DCMAKE_C_COMPILER_LAUNCHER=$work/launcher"
  "-DCMAKE_C_LINKER_LAUNCHER=$work/launcher")
printf 'set(SOFTLINE_ABI_VERSION "%s" CACHE STRING "stale ABI")\n' "$stale_abi" > "$work/stale-abi.cmake"
"$cmake" -C "$work/stale-abi.cmake" "${args[@]}"
"$cmake" --build "$build" --parallel 4
symbol=sl_output_stream_write_quoted_prompt
map="$build/softline.exports.map"
library="$build/libsoftline.so"
grep -q "$symbol" "$map"
"$nm" -D --defined-only "$library" | grep "$symbol"
soname=$("$readelf" -d "$library" | sed -n '/(SONAME)/s/.*\[\([^]]*\)\].*/\1/p')
[[ "$soname" == "libsoftline.so.$abi" ]] || {
  echo "ERROR: stale ABI cache produced SONAME '$soname'; expected libsoftline.so.$abi" >&2
  exit 1
}
if grep -q '^SOFTLINE_ABI_VERSION:' "$build/CMakeCache.txt"; then
  echo 'ERROR: shared-library ABI must not remain a cache option' >&2
  exit 1
fi
"$cmake" --install "$build" --prefix "$work/install"
[[ -f "$work/install/share/doc/softline/docs/softline-spec.md" &&
   -f "$work/install/share/doc/softline/lua/README.md" ]]
[[ -L "$work/install/lib/libsoftline.so.$abi" &&
   ! -e "$work/install/lib/libsoftline.so.$stale_abi" ]]
grep -Fq "\"abi_version\": \"$abi\"" "$work/install/share/softline/package-metadata.json"
: > "$COMPILER_CALLS"
"$cmake" "${args[@]}"
"$cmake" --build "$build" --parallel 4
[[ ! -s "$COMPILER_CALLS" ]]
namespace="$build/private-cares/softline_cares_namespace.h"
cp -p "$namespace" "$work/namespace"
rm "$namespace"
"$cmake" --build "$build" -- -t query private-cares/softline_cares_namespace.h > "$work/recovery-plan"
grep -q 'input: CUSTOM_COMMAND' "$work/recovery-plan"
"$cmake" --build "$build" --target sl_cares_namespace
cmp "$work/namespace" "$namespace"
# The regenerated header is identical. Avoid compiling the resolver again
# when checking that changing the export map relinks the library.
touch -r "$work/namespace" "$namespace"
sed -i "/^$symbol\$/d" "$work/source/cmake/softline.exports"
"$cmake" --build "$build" --parallel 1
if grep -q "$symbol" "$map"; then echo 'ERROR: map did not regenerate' >&2; exit 1; fi
if "$nm" -D --defined-only "$library" | grep "$symbol"; then echo 'ERROR: removed export is still public' >&2; exit 1; fi
echo 'Stale-cache ABI, installed metadata, no-op reuse, generated-header ownership and export reconfiguration passed.'
