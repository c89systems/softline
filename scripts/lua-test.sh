#!/usr/bin/env bash
case ${BASH_SOURCE[0]} in
  */*) source "${BASH_SOURCE[0]%/*}/require-host-bash.sh" ;;
  *) source ./require-host-bash.sh ;;
esac || exit $?
set -euo pipefail
source "${BASH_SOURCE[0]%/*}/require-workspace.sh" || exit $?

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
LUA_TREE="${ROOT_DIR}/build/luarocks"
SDK_DIR="${ROOT_DIR}/build/lua-sdk"
ROCKSPEC="${ROOT_DIR}/build/lua-rockspec/softline-$(bash "${ROOT_DIR}/scripts/release_version.sh")-1.rockspec"
INSTALL_LIBDIR="${SOFTLINE_LUA_INSTALL_LIBDIR:-}"
INSTALL_LIBDIR_ARG="-DCMAKE_INSTALL_LIBDIR=${INSTALL_LIBDIR:-lib}"

(
  cd "${ROOT_DIR}"
  "${ROOT_DIR}/scripts/terminal-test-env.sh" cmake --preset debug \
    "${INSTALL_LIBDIR_ARG}" -DSL_BUILD_TESTS=ON -DSL_BUILD_EXAMPLES=ON
  cmake --build --preset debug --target softline_static softline_shared \
    test_clipboard_image clipboard_fixture
)
rm -rf "${SDK_DIR}"
cmake --install "${ROOT_DIR}/build/debug" --prefix "${SDK_DIR}"

if [ -d "${SDK_DIR}/lib/pkgconfig" ]; then
  SDK_LIB_DIR="${SDK_DIR}/lib"
elif [ -d "${SDK_DIR}/lib64/pkgconfig" ]; then
  SDK_LIB_DIR="${SDK_DIR}/lib64"
else
  echo "ERROR: Lua SDK install missing lib/pkgconfig or lib64/pkgconfig" >&2
  exit 1
fi

"${ROOT_DIR}/scripts/render_lua_rockspec.sh" "${ROCKSPEC}" >/dev/null
bash "${ROOT_DIR}/scripts/build-local-lua.sh" "${SDK_LIB_DIR}"
SOFTLINE_LUA_INCDIR="$(cat "${ROOT_DIR}/build/local-lua/lua-source-dir.txt")"
export SOFTLINE_LUA_INCDIR
rm -rf "${LUA_TREE}"
NATIVE_TARGET="$("${ROOT_DIR}/scripts/native-linux-target.sh")"
BOOTLIN_ENV="$("${ROOT_DIR}/scripts/cpkt-toolchains.sh" env "${NATIVE_TARGET}")"
eval "${BOOTLIN_ENV}"
SOFTLINE_LUA_CC="${CC}"
export CC LD AR RANLIB SOFTLINE_LUA_CC
(
  cd "${ROOT_DIR}"
  PKG_CONFIG_PATH="${SDK_LIB_DIR}/pkgconfig${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}" \
    SOFTLINE_INCLUDE_DIR="${SDK_DIR}/include" SOFTLINE_LIB_DIR="${SDK_LIB_DIR}" \
    SOFTLINE_DIR="${SDK_DIR}" \
    luarocks --tree "${LUA_TREE}" make "${ROCKSPEC}"
)

SOFTLINE_LUA_ENV="$("${ROOT_DIR}/scripts/lua-env.sh")"
eval "${SOFTLINE_LUA_ENV}"
mkdir -p "${ROOT_DIR}/build/lua-test-history"
SOFTLINE_HISTORY_DIR="${ROOT_DIR}/build/lua-test-history"
export SOFTLINE_HISTORY_DIR
cmake -S "${ROOT_DIR}/cmake/lua-tests" -B "${ROOT_DIR}/build/lua-tests" -G Ninja \
  -DSOFTLINE_LUA_EXECUTABLE="$(command -v lua)" -DSOFTLINE_LUA_DEBUG=OFF
exec ctest --test-dir "${ROOT_DIR}/build/lua-tests" --stop-on-failure --output-on-failure --no-tests=error
