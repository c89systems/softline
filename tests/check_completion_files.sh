#!/usr/bin/env bash
set -euo pipefail
root=$1
mode=$2
shift 2
mkdir -p "$root/build"
work=$(mktemp -d "$root/build/completion-files.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
fixture="$work/files"
mkdir -p "$fixture/tree/nested" "$fixture/tree/empty" "$fixture/.hidden" \
  "$fixture/.git/objects" "$fixture/tree/.git" "$fixture/space dir" "$fixture/only"
touch "$fixture/README.md" "$fixture/CTestTestfile.cmake" \
  "$fixture/tree/nested/source.c" "$fixture/tree/nested/space name.txt" \
  "$fixture/.hidden/config" "$fixture/tree/.env" \
  "$fixture/.git/objects/excluded" "$fixture/tree/.git/excluded" \
  "$fixture/space dir/other.txt" "$fixture/only/file.txt" "$fixture/tree/nested/café.c" \
  "$fixture/tree/nested/"$'line\nbreak.txt' "$fixture/tree/empty/.git"
ln -s ../.. "$fixture/tree/nested/loop"
ln -s missing "$fixture/dangling"
ln -s .git "$fixture/git-link"
export SOFTLINE_TEST_COMPLETION_DIR="$fixture"
cd "$fixture"
case "$mode" in
  inventory)
    for prefix in '@' '@tree/' '@tree/nested/' '@tree/empty/' '@.hidden/' '@space dir/' '@only/' '@README.md'; do
      "$@" "$prefix" > "$work/actual"
      case "$prefix" in
        '@') printf '%s\0' '@README.md' '@CTestTestfile.cmake' '@tree/' '@.hidden/' '@space dir/' '@only/' '@dangling' '@git-link' ;;
        '@tree/') printf '%s\0' '@tree/nested/' '@tree/empty/' '@tree/.env' ;;
        '@tree/nested/') printf '%s\0' '@tree/nested/source.c' '@tree/nested/space name.txt' '@tree/nested/loop' '@tree/nested/café.c' $'@tree/nested/line\nbreak.txt' ;;
        '@tree/empty/'|'@README.md') : ;;
        '@.hidden/') printf '%s\0' '@.hidden/config' ;;
        '@space dir/') printf '%s\0' '@space dir/other.txt' ;;
        '@only/') printf '%s\0' '@only/file.txt' ;;
      esac | LC_ALL=C sort -z > "$work/expected"
      LC_ALL=C sort -z "$work/actual" > "$work/sorted"
      cmp "$work/expected" "$work/sorted"
    done
    ;;
  interactive) "$@" ;;
  *) echo 'ERROR: unknown file completion test mode' >&2; exit 2 ;;
esac
