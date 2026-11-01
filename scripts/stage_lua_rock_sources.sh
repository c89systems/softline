#!/usr/bin/env bash
case ${BASH_SOURCE[0]} in
  */*) source "${BASH_SOURCE[0]%/*}/require-host-bash.sh" ;;
  *) source ./require-host-bash.sh ;;
esac || exit $?
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(bash "${ROOT_DIR}/scripts/release_version.sh")"
STAGE_ROOT="${1:-${ROOT_DIR}/build/lua-release/softline-lua-${VERSION}}"
SOFTLINE_GENERATED_PATH="$STAGE_ROOT" source "${BASH_SOURCE[0]%/*}/require-workspace.sh" || exit $?

rm -rf "${STAGE_ROOT}"
mkdir -p "${STAGE_ROOT}/lua" "${STAGE_ROOT}/scripts" \
  "${STAGE_ROOT}/include/softline" "${STAGE_ROOT}/tests" \
  "${STAGE_ROOT}/examples"

cp "${ROOT_DIR}/LICENSE" "${STAGE_ROOT}/"
cp "${ROOT_DIR}/README.md" "${STAGE_ROOT}/"
cp "${ROOT_DIR}/softline.rockspec.in" "${STAGE_ROOT}/"
cp "${ROOT_DIR}/lua/README.md" "${STAGE_ROOT}/lua/"
cp "${ROOT_DIR}/lua/softline_lua.c" "${STAGE_ROOT}/lua/"
cp "${ROOT_DIR}/lua/softline_queue_backend_lua.h" "${STAGE_ROOT}/lua/"
cp "${ROOT_DIR}/scripts/build_lua_rock.sh" "${STAGE_ROOT}/scripts/"
cp "${ROOT_DIR}/scripts/require-host-bash.sh" "${STAGE_ROOT}/scripts/"
cp "${ROOT_DIR}/scripts/release_version.sh" "${STAGE_ROOT}/scripts/"
cp "${ROOT_DIR}/scripts/render_lua_rockspec.sh" "${STAGE_ROOT}/scripts/"
cp "${ROOT_DIR}/include/softline/softline.h" "${STAGE_ROOT}/include/softline/"
cp "${ROOT_DIR}/tests/lua_smoke.lua" "${STAGE_ROOT}/tests/"
cp "${ROOT_DIR}/tests/lua_readline.lua" "${STAGE_ROOT}/tests/"
cp "${ROOT_DIR}/tests/lua_watch_file_gc.lua" "${STAGE_ROOT}/tests/"
cp "${ROOT_DIR}/examples/simple.lua" "${STAGE_ROOT}/examples/"
cp "${ROOT_DIR}/examples/chat.lua" "${STAGE_ROOT}/examples/"

cat > "${STAGE_ROOT}/RELEASE_MANIFEST" <<'EOF'
LICENSE
README.md
softline.rockspec.in
lua/README.md
lua/softline_lua.c
lua/softline_queue_backend_lua.h
scripts/build_lua_rock.sh
scripts/require-host-bash.sh
scripts/release_version.sh
scripts/render_lua_rockspec.sh
include/softline/softline.h
tests/lua_smoke.lua
tests/lua_readline.lua
tests/lua_watch_file_gc.lua
examples/simple.lua
examples/chat.lua
VERSION
RELEASE_MANIFEST
EOF
echo "${VERSION}" > "${STAGE_ROOT}/VERSION"
echo "${STAGE_ROOT}"
