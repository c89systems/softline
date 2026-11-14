#!/usr/bin/env bash
case ${BASH_SOURCE[0]} in
  */*) source "${BASH_SOURCE[0]%/*}/require-host-bash.sh" ;;
  *) source ./require-host-bash.sh ;;
esac || exit $?
set -euo pipefail
source "${BASH_SOURCE[0]%/*}/require-workspace.sh" || exit $?

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
LUA_TREE="${ROOT_DIR}/build/luarocks-debug"
ROCKSPEC="${ROOT_DIR}/build/lua-debug-rockspec/softline-$(bash "${ROOT_DIR}/scripts/release_version.sh")-1.rockspec"
MODE="${1:-test}"

if [ "${MODE}" = "env" ]; then
  exec "${ROOT_DIR}/scripts/lua-debug-env.sh"
fi

(
  cd "${ROOT_DIR}"
  if [ "${MODE}" = test ]; then
    "${ROOT_DIR}/scripts/terminal-test-env.sh" cmake --preset debug \
      -DSL_BUILD_TESTS=ON -DSL_BUILD_EXAMPLES=ON
  else
    cmake --preset debug
  fi
  cmake --build --preset debug
)

"${ROOT_DIR}/scripts/render_lua_rockspec.sh" "${ROCKSPEC}" >/dev/null
bash "${ROOT_DIR}/scripts/build-local-lua.sh" "${ROOT_DIR}/build/debug" local-lua-debug
SOFTLINE_LUA_INCDIR="$(cat "${ROOT_DIR}/build/local-lua-debug/lua-source-dir.txt")"
export SOFTLINE_LUA_INCDIR
rm -rf "${LUA_TREE}"
NATIVE_TARGET="$("${ROOT_DIR}/scripts/native-linux-target.sh")"
BOOTLIN_ENV="$("${ROOT_DIR}/scripts/cpkt-toolchains.sh" env "${NATIVE_TARGET}")"
eval "${BOOTLIN_ENV}"
SOFTLINE_LUA_CC="${CC}"
export CC LD AR RANLIB SOFTLINE_LUA_CC
(
  cd "${ROOT_DIR}"
  SOFTLINE_INCLUDE_DIR="${ROOT_DIR}/include" \
    SOFTLINE_LIB_DIR="${ROOT_DIR}/build/debug" \
    luarocks --tree "${LUA_TREE}" make "${ROCKSPEC}"
)

SOFTLINE_LUA_ENV="$("${ROOT_DIR}/scripts/lua-debug-env.sh")"
eval "${SOFTLINE_LUA_ENV}"

case "${MODE}" in
  test)
    mkdir -p "${ROOT_DIR}/build/lua-debug-history"
    SOFTLINE_HISTORY_DIR="${ROOT_DIR}/build/lua-debug-history"
    export SOFTLINE_HISTORY_DIR
    cmake -S "${ROOT_DIR}/cmake/lua-tests" -B "${ROOT_DIR}/build/lua-debug-tests" -G Ninja \
      -DSOFTLINE_LUA_EXECUTABLE="$(command -v lua)" -DSOFTLINE_LUA_DEBUG=ON
    ctest --test-dir "${ROOT_DIR}/build/lua-debug-tests" --stop-on-failure --output-on-failure --no-tests=error
    ;;
  simple)
    exec lua "${ROOT_DIR}/examples/simple.lua"
    ;;
  chat)
    exec lua "${ROOT_DIR}/examples/chat.lua"
    ;;
  *)
    echo "usage: $0 [env|test|simple|chat]" >&2
    exit 2
    ;;
esac
