#!/usr/bin/env bash
case ${BASH_SOURCE[0]} in
  */*) source "${BASH_SOURCE[0]%/*}/require-host-bash.sh" ;;
  *) source ./require-host-bash.sh ;;
esac || exit $?
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="$(bash "${ROOT_DIR}/scripts/release_version.sh")"
DIST_DIR="${SOFTLINE_DIST_DIR:-${ROOT_DIR}/dist}"
OUT="${DIST_DIR}/softline-${VERSION}-1.rockspec"
SOURCE_URL="${SOURCE_URL:-https://github.com/c89systems/softline/releases/download/v${VERSION}/softline-lua-${VERSION}.tar.gz}"
SOURCE_DIR="${SOURCE_DIR:-softline-lua-${VERSION}}"

mkdir -p "${DIST_DIR}"
SOURCE_URL="${SOURCE_URL}" SOURCE_DIR="${SOURCE_DIR}" \
  "${ROOT_DIR}/scripts/render_lua_rockspec.sh" "${OUT}" >/dev/null
echo "${OUT}"
