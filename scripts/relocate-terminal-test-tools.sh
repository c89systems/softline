#!/usr/bin/env bash
case ${BASH_SOURCE[0]} in
  */*) source "${BASH_SOURCE[0]%/*}/require-host-bash.sh" ;;
  *) source ./require-host-bash.sh ;;
esac || exit $?
set -euo pipefail
ROOT_DIR=$(cd "${BASH_SOURCE[0]%/*}/.." && pwd)
[[ $# -eq 1 || ( $# -eq 2 && ${2:-} == --verify ) ]] || { echo 'Usage: relocate-terminal-test-tools.sh install-root [--verify]' >&2; exit 2; }
root=$(cd "$1" && pwd -P)
SOFTLINE_GENERATED_PATH="$root" source "${BASH_SOURCE[0]%/*}/require-workspace.sh" || exit $?
if (($# == 1)); then
  server="$root/usr/bin/Xvfb"
  offset=
  while IFS=: read -r candidate ignored; do
    next=$(od -An -tu1 -j "$((candidate + 8))" -N1 "$server" | tr -d ' \n')
    if [[ "$next" == 0 ]]; then
      [[ -z "$offset" ]] || { echo 'ERROR: pinned Xvfb keyboard prefix is ambiguous' >&2; exit 1; }
      offset=$candidate
    fi
  done < <(LC_ALL=C grep -abo '/usr/bin' "$server" || true)
  [[ -n "$offset" ]] || { echo 'ERROR: pinned Xvfb keyboard compiler prefix changed' >&2; exit 1; }
  dd if=/dev/zero of="$server" bs=1 seek="$offset" count=9 conv=notrunc status=none
fi
# Verify all required library names and imported libc versions. Resolve inspection
# tooling only for ELF inputs; synthetic text fixtures need no compiler collection.
declare -A providers=() provided_versions=()
elf_files=()
while IFS= read -r -d '' file; do
  providers[${file##*/}]=1
  magic=
  LC_ALL=C IFS= read -r -n 4 -d '' magic < "$file" || :
  if [[ "$magic" == $'\x7fELF' ]]; then elf_files+=("$file"); fi
done < <(find "$root" -type f -print0)
while IFS= read -r -d '' file; do
  [[ -f "$file" ]] || continue
  resolved=$(readlink -f "$file")
  [[ "$resolved" == "$root/"* ]] || continue
  providers[${file##*/}]=1
done < <(find "$root" -type l -print0)
readelf=${SOFTLINE_READELF:-}
resolve_readelf() {
  [[ -z "$readelf" ]] || return 0
  description=$("$ROOT_DIR/scripts/cpkt-toolchains.sh" discover x86_64-linux-gnu)
  readelf=$(sed -n 's/^readelf=//p' <<< "$description")
  [[ -x "$readelf" ]] || { echo 'ERROR: prepared lifecycle readelf is required for terminal runtime verification' >&2; exit 1; }
}
libc="$root/usr/lib/x86_64-linux-gnu/libc.so.6"
if [[ -f "$libc" ]]; then
  resolve_readelf
  versions=$("$readelf" --version-info "$libc")
  while IFS= read -r version; do
    [[ -z "$version" ]] || provided_versions[$version]=1
  done < <(awk '/Version definition section/ { definitions=1; next } /Version needs section/ { definitions=0 } definitions { for(i=1;i<NF;i++) if($i=="Name:" && $(i+1) ~ /^GLIBC_/) print $(i+1) }' <<< "$versions")
fi
for file in "${elf_files[@]}"; do
  resolve_readelf
  dynamic=$("$readelf" -d "$file")
  versions=$("$readelf" --version-info "$file")
  while IFS= read -r version; do
    [[ -n ${provided_versions[$version]:-} ]] || {
      echo "ERROR: ${file#$root/}: required libc version $version is not staged" >&2; exit 1;
    }
  done < <(awk '/Version needs section/ { needs=1; next } needs { for(i=1;i<NF;i++) if($i=="Name:" && $(i+1) ~ /^GLIBC_/) print $(i+1) }' <<< "$versions")
  while IFS= read -r library; do
    [[ -n ${providers[$library]:-} ]] || {
      echo "ERROR: ${file#$root/}: required library $library is not staged" >&2; exit 1;
    }
  done < <(sed -n 's/.*(NEEDED).*\[\([^]]*\)\].*/\1/p' <<< "$dynamic")
done
