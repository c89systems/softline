#!/usr/bin/env bash
case ${BASH_SOURCE[0]} in
  */*) source "${BASH_SOURCE[0]%/*}/require-host-bash.sh" ;;
  *) source ./require-host-bash.sh ;;
esac || exit $?
set -euo pipefail
source "${BASH_SOURCE[0]%/*}/require-workspace.sh" || exit $?
ROOT_DIR=$(cd "${BASH_SOURCE[0]%/*}/.." && pwd)
mode=${1:-smoke}
case "$mode" in smoke|long) ;; *) echo 'Usage: fuzz.sh [smoke|long]' >&2; exit 2 ;; esac
# Validate owned inputs before compiler discovery or provisioning.
[[ -s "$ROOT_DIR/fuzz/corpus/basic" ]] || { echo 'ERROR: fuzz/corpus/basic must be a nonempty seed' >&2; exit 1; }
cd "$ROOT_DIR"
"$ROOT_DIR/scripts/cpkt-aflpp.sh" ensure
resolved_env=$("$ROOT_DIR/scripts/cpkt-aflpp.sh" env) || exit $?
eval "$resolved_env"
afl_fuzz="$CPKT_AFLPP_ROOT/bin/afl-fuzz"
afl_showmap="$CPKT_AFLPP_ROOT/bin/afl-showmap"
cmake --preset fuzz
cmake --build --preset fuzz
if [[ "$mode" == long ]]; then
  exec "$afl_fuzz" -i fuzz/corpus -o build/fuzz-findings -- ./build/fuzz/softline_stdin_fuzz
fi
timeout 15 "$afl_showmap" -q -o /dev/null -- ./build/fuzz/softline_stdin_fuzz < fuzz/corpus/basic
