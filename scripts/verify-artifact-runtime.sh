#!/usr/bin/env bash
case ${BASH_SOURCE[0]} in
  */*) source "${BASH_SOURCE[0]%/*}/require-host-bash.sh" ;;
  *) source ./require-host-bash.sh ;;
esac || exit $?
set -euo pipefail
source "${BASH_SOURCE[0]%/*}/require-workspace.sh" || exit $?
ROOT_DIR=$(cd "${BASH_SOURCE[0]%/*}/.." && pwd)
(($# >= 1)) || { echo 'Usage: verify-artifact-runtime.sh path [--readelf tool] [--manifest file]' >&2; exit 2; }
path=$1; shift
[[ "$path" == /* ]] || path="$PWD/$path"
readelf=${SOFTLINE_READELF:-}
manifest=
while (($#)); do
  (($# >= 2)) || { echo 'ERROR: missing scanner option value' >&2; exit 2; }
  case "$1" in --readelf) readelf=$2 ;; --manifest) manifest=$2 ;; *) echo "ERROR: unknown scanner option: $1" >&2; exit 2 ;; esac
  shift 2
done
[[ -e "$path" ]] || { echo "ERROR: missing artifact path: $path" >&2; exit 1; }
mkdir -p "$ROOT_DIR/build"
scratch=$(mktemp -d "$ROOT_DIR/build/artifact-runtime.XXXXXX")
trap 'rm -rf "$scratch"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cache=${XDG_CACHE_HOME:-${HOME:-}/.cache}
forbidden=("$ROOT_DIR" "${HOME:-}" "${CPKT_TOOLCHAIN_CACHE:-$cache/cpkt/toolchains}"
  "${CPKT_DEPENDENCY_CACHE:-$cache/cpkt/deps}" "${CPKT_TOOLCHAIN_ROOT:-}" "${CPKT_TOOLCHAIN_SYSROOT:-}")
fail() { echo "ERROR: $*" >&2; exit 1; }
scan_paths() {
  local file=$1 label=$2 needle collection_file=$1
  for needle in "${forbidden[@]}"; do
    [[ -z "$needle" ]] || ! LC_ALL=C grep -aFq -- "$needle" "$file" || fail "$label: forbidden local/cache path $needle"
  done
  # Shell source fixtures refer to a variable-based cache, not an absolute
  # builder path. Only remove those template references from this check.
  case "$label" in
    scripts/test-*.sh|*/scripts/test-*.sh|tests/lifecycle/*.sh|*/tests/lifecycle/*.sh)
      sed -E 's@\$\{[A-Za-z_][A-Za-z0-9_]*\}/[-/A-Za-z0-9_.+]*|\$[A-Za-z_][A-Za-z0-9_]*\/[-/A-Za-z0-9_.+]*@@g' "$file" > "$scratch/collection-paths"
      collection_file="$scratch/collection-paths"
      ;;
  esac
  if LC_ALL=C grep -aEq '/[^[:space:]"]*--(glibc|musl)--stable-[0-9][^[:space:]"]*/' "$collection_file"; then
    fail "$label: pinned Bootlin collection path"
  fi
}
inspect_elf() {
  local file=$1 label=$2 output value entry cache_file description soname
  if [[ -z "$readelf" ]]; then
    cache_file="$ROOT_DIR/build/x86_64-linux-gnu-release/CMakeCache.txt"
    if [[ -f "$cache_file" ]]; then
      readelf=$(sed -n 's/^CMAKE_READELF:[^=]*=//p' "$cache_file")
    elif [[ -x "$ROOT_DIR/scripts/cpkt-toolchains.sh" ]]; then
      description=$("$ROOT_DIR/scripts/cpkt-toolchains.sh" discover x86_64-linux-gnu)
      readelf=$(sed -n 's/^readelf=//p' <<< "$description")
    fi
  fi
  [[ -n "$readelf" ]] || fail "$label: readelf required for ELF artifact verification"
  if ! output=$(LC_ALL=C "$readelf" -W -d -l "$file" 2> "$scratch/elf.err") || [[ -s "$scratch/elf.err" ]]; then
    fail "$label: ELF inspection failed: $(cat "$scratch/elf.err")"
  fi
  while IFS= read -r value; do
    [[ "$value" != :* && "$value" != *: && "$value" != *::* ]] || fail "$label: non-relocatable ELF runtime path '$value'"
    IFS=: read -r -a entries <<< "$value"
    for entry in "${entries[@]}"; do
      [[ "$entry" =~ ^\$ORIGIN(/[A-Za-z0-9_.+-]+)*$ ]] || fail "$label: non-relocatable ELF runtime path '$entry'"
    done
  done < <(sed -n '/(RPATH)\|(RUNPATH)/s/.*\[\([^]]*\)\].*/\1/p' <<< "$output")
  while IFS= read -r value; do
    [[ "$value" != */* && ! "$value" =~ lib(asan|ubsan|tsan|lsan|msan) ]] || fail "$label: forbidden ELF dependency/SONAME $value"
  done < <(sed -n '/(NEEDED)\|(SONAME)/s/.*\[\([^]]*\)\].*/\1/p' <<< "$output")
  while IFS= read -r value; do
    [[ "$value" =~ ^/(lib|lib64)/(ld-linux[^/]*\.so\.[0-9]+|ld-musl-[^/]+\.so\.1)$ ]] || fail "$label: non-system ELF interpreter $value"
  done < <(sed -n 's/.*Requesting program interpreter: \([^]]*\)\].*/\1/p' <<< "$output")
  if [[ "${file##*/}" == libsoftline.so* ]]; then
    soname=$(sed -n '/(SONAME)/s/.*\[\([^]]*\)\].*/\1/p' <<< "$output")
    [[ "$soname" == "libsoftline.so.${SOFTLINE_ABI_VERSION:-5}" ]] || fail "$label: missing or incorrect softline SONAME"
  fi
}
scan() {
  local file=$1 label=$2 depth=${3:-0} magic extraction listing member link
  ((depth <= 16)) || fail "$label: archive nesting limit exceeded"
  if [[ -L "$file" ]]; then
    link=$(readlink "$file")
    [[ "$link" != /* ]] || fail "$label: absolute archive link"
    printf '%s' "$link" > "$scratch/link"
    scan_paths "$scratch/link" "$label"
    return
  fi
  [[ -f "$file" ]] || return 0
  scan_paths "$file" "$label"
  magic=$(od -An -tx1 -N4 "$file" | tr -d ' \n')
  if [[ "$magic" == 7f454c46 ]]; then inspect_elf "$file" "$label"; return; fi
  case "$file" in
    *.tar.gz|*.tgz|*.tar.xz|*.tar|*.zip|*.rock)
      extraction=$(mktemp -d "$scratch/unpack.XXXXXX")
      listing=$(cmake -E tar tf "$file") || fail "$label: archive inspection failed"
      while IFS= read -r member; do
        [[ "$member" != /* && "/$member/" != *'/../'* ]] || fail "$label!$member: unsafe archive path"
      done <<< "$listing"
      listing=$(cmake -E tar tvf "$file") || fail "$label: archive inspection failed"
      while IFS= read -r member; do
        case "$member" in
          -*|d*) ;;
          l*|h*)
            link=${member##* -> }
            [[ "$link" != /* && "/$link/" != *'/../'* ]] || fail "$label: unsafe archive link $link"
            ;;
          *) fail "$label: unsupported archive entry type" ;;
        esac
      done <<< "$listing"
      (cd "$extraction"; cmake -E tar xf "$file") || fail "$label: archive extraction failed"
      while IFS= read -r -d '' member; do
        scan "$member" "$label!${member#$extraction/}" "$((depth + 1))"
      done < <(find "$extraction" \( -type f -o -type l \) -print0)
      rm -rf "$extraction"
      ;;
    *.gz)
      extraction=$(mktemp -d "$scratch/gunzip.XXXXXX")
      gzip -dc "$file" > "$extraction/$(basename "${file%.gz}")" || fail "$label: gzip extraction failed"
      scan "$extraction/$(basename "${file%.gz}")" "${label%.gz}" "$((depth + 1))"
      rm -rf "$extraction"
      ;;
  esac
}
if [[ -n "$manifest" ]]; then
  scan_paths "$manifest" "${manifest##*/}"
  declare -A seen=()
  count=0
  while read -r digest member extra || [[ -n "$digest$member$extra" ]]; do
    [[ "$digest" =~ ^[a-f0-9]{64}$ && -z ${extra:-} && "$member" != */* && "$member" != . && "$member" != .. && -z ${seen[$member]:-} ]] || fail 'invalid or duplicate checksum entry'
    [[ -f "$path/$member" && ! -L "$path/$member" ]] || fail "missing or unsafe checksum artifact: $member"
    actual=$(sha256sum "$path/$member" | awk '{print $1}')
    [[ "$actual" == "$digest" ]] || fail "checksum mismatch: $member"
    seen[$member]=1
    count=$((count + 1))
    scan "$path/$member" "$member"
  done < "$manifest"
  ((count)) || fail 'empty checksum manifest'
elif [[ -d "$path" ]]; then
  while IFS= read -r -d '' file; do scan "$file" "${file#$path/}"; done < <(find "$path" \( -type f -o -type l \) -print0)
else
  [[ "$path" == /* ]] || path="$PWD/$path"
  scan "$path" "${path##*/}"
fi
echo 'Artifact privacy and ELF runtime verification passed.'
