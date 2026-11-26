#!/usr/bin/env bash
case ${BASH_SOURCE[0]} in
  */*) source "${BASH_SOURCE[0]%/*}/require-host-bash.sh" ;;
  *) source ./require-host-bash.sh ;;
esac || exit $?
set -euo pipefail
source "${BASH_SOURCE[0]%/*}/require-workspace.sh" || exit $?
ROOT_DIR=$(cd "${BASH_SOURCE[0]%/*}/.." && pwd)
mode=${1:-build}
preset=${2:-debug}
if (($#)); then shift; fi
if (($#)); then shift; fi
case "$mode" in configure|build|test) ;; *) echo 'Usage: build.sh [configure|build|test] [preset] [CTest options]' >&2; exit 2 ;; esac
cd "$ROOT_DIR"
case "$preset" in
  debug|asan|valgrind)
    target=$("$ROOT_DIR/scripts/native-linux-target.sh")
    "$ROOT_DIR/scripts/cpkt-toolchains.sh" ensure "$target" >/dev/null
    ;;
  *-release) "$ROOT_DIR/scripts/cpkt-toolchains.sh" ensure "${preset%-release}" >/dev/null ;;
  fuzz) "$ROOT_DIR/scripts/cpkt-aflpp.sh" ensure ;;
esac
cmake --preset "$preset"
if [[ "$mode" == configure ]]; then exit 0; fi
cmake --build --preset "$preset"
if [[ "$mode" == test ]]; then
  exec ctest --preset "$preset" --stop-on-failure --output-on-failure --no-tests=error "$@"
fi
