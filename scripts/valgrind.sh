#!/usr/bin/env bash
case ${BASH_SOURCE[0]} in
  */*) source "${BASH_SOURCE[0]%/*}/require-host-bash.sh" ;;
  *) source ./require-host-bash.sh ;;
esac || exit $?
set -euo pipefail
source "${BASH_SOURCE[0]%/*}/require-workspace.sh" || exit $?
ROOT_DIR=$(cd "${BASH_SOURCE[0]%/*}/.." && pwd)
command -v valgrind >/dev/null || { echo 'ERROR: install host Valgrind for the native memory-check gate' >&2; exit 1; }
cd "$ROOT_DIR"
"$ROOT_DIR/scripts/build.sh" build valgrind
cd "$ROOT_DIR/build/debug"
exec ctest --stop-on-failure --output-on-failure --no-tests=error \
  -L memcheck \
  -T memcheck --overwrite "MemoryCheckCommand=$(command -v valgrind)" \
  --overwrite 'MemoryCheckCommandOptions=--leak-check=full --track-origins=yes --error-exitcode=1 --trace-children=yes' "$@"
