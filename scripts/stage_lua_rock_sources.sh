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
mkdir -p "${STAGE_ROOT}"
files=(
  LICENSE README.md softline.rockspec.in lua/README.md
  lua/softline_lua.c lua/softline_image_source_lua.h
  lua/softline_queue_backend_lua.h lua/softline_terminal_lua.h
  scripts/build_lua_rock.sh scripts/require-host-bash.sh
  scripts/release_version.sh scripts/render_lua_rockspec.sh
  include/softline/softline.h
  tests/lua_smoke.lua tests/lua_readline.lua tests/lua_watch_file_gc.lua
  examples/simple.lua examples/chat.lua examples/completion.lua
)
for guide in "${ROOT_DIR}"/docs/*.md; do
  files+=("docs/${guide##*/}")
done
for file in "${files[@]}"; do
  if [[ "$file" == */* ]]; then
    mkdir -p "${STAGE_ROOT}/${file%/*}"
  fi
  cp "${ROOT_DIR}/${file}" "${STAGE_ROOT}/${file}"
done
printf '%s\n' "${files[@]}" VERSION RELEASE_MANIFEST > "${STAGE_ROOT}/RELEASE_MANIFEST"
echo "${VERSION}" > "${STAGE_ROOT}/VERSION"
echo "${STAGE_ROOT}"
