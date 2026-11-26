#!/usr/bin/env bash
case ${BASH_SOURCE[0]} in
  */*) source "${BASH_SOURCE[0]%/*}/require-host-bash.sh" ;;
  *) source ./require-host-bash.sh ;;
esac || exit $?
set -euo pipefail
source "${BASH_SOURCE[0]%/*}/require-workspace.sh" || exit $?
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
# Other hosts retain the existing host-tool discovery behavior.
if [ "$(uname -s):$(uname -m)" != Linux:x86_64 ]; then
  if [ "$#" -gt 0 ]; then exec "$@"; fi
  echo "Pinned terminal test tools require native x86_64 Linux" >&2
  exit 1
fi
"${ROOT_DIR}/scripts/cpkt-toolchains.sh" ensure x86_64-linux-gnu >/dev/null
cmake -S "${ROOT_DIR}/cmake/terminal-tests" -B "${ROOT_DIR}/build/terminal-test-tools"
install="$(cat "${ROOT_DIR}/build/terminal-test-tools/install-root.txt")"
# Apply cached runtime libraries only to terminal processes. Host build tools
# and shell utilities must keep their own runtime, even on a newer workstation.
export SOFTLINE_TERMINAL_TEST_ROOT="${install}"
mkdir -p "${ROOT_DIR}/build/terminal-test-tools/bin"
for tool in Xvfb xauth xkbcomp; do
  cat > "${ROOT_DIR}/build/terminal-test-tools/bin/${tool}" <<'EOF'
#!/bin/sh
root="$(CDPATH= cd -- "${0%/*}/../../.." && pwd)"
install="${SOFTLINE_TERMINAL_TEST_ROOT:-${root}/.cache/deps/x86_64-linux-gnu/terminal-tests/install}"
exec "${root}/scripts/terminal-test-runtime.sh" "${install}/usr/bin/${0##*/}" "$@"
EOF
  chmod +x "${ROOT_DIR}/build/terminal-test-tools/bin/${tool}"
done
export PATH="${ROOT_DIR}/build/terminal-test-tools/bin:${install}/usr/bin:${PATH}"
export CMAKE_LIBRARY_PATH="${install}/usr/lib/x86_64-linux-gnu${CMAKE_LIBRARY_PATH:+:${CMAKE_LIBRARY_PATH}}"
export FONTCONFIG_PATH="${install}/etc/fonts"
export XKB_CONFIG_ROOT="${install}/usr/share/X11/xkb"
export NO_AT_BRIDGE=1
mkdir -p "${ROOT_DIR}/build/terminal-test-tools/tmp" "${ROOT_DIR}/build/terminal-test-tools/font-cache"
export TMPDIR="${ROOT_DIR}/build/terminal-test-tools/tmp"
export FONTCONFIG_FILE="${ROOT_DIR}/build/terminal-test-tools/fonts.conf"
cat > "${FONTCONFIG_FILE}" <<EOF
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
<fontconfig><dir>${install}/usr/share/fonts</dir><cachedir>${ROOT_DIR}/build/terminal-test-tools/font-cache</cachedir></fontconfig>
EOF
if [ "$#" -gt 0 ]; then
  exec "$@"
fi
printf 'Terminal test tools ready. Run: make test-terminal\n'
