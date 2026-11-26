#!/usr/bin/env bash
case ${BASH_SOURCE[0]} in
  */*) source "${BASH_SOURCE[0]%/*}/require-host-bash.sh" ;;
  *) source ./require-host-bash.sh ;;
esac || exit $?
set -euo pipefail
source "${BASH_SOURCE[0]%/*}/require-workspace.sh" || exit $?

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DIST_DIR="${SOFTLINE_DIST_DIR:-${ROOT_DIR}/dist}"
. "${ROOT_DIR}/scripts/release-targets.sh"

VERSION="$(bash "${ROOT_DIR}/scripts/release_version.sh")"
REQUIRE_DARWIN="${SOFTLINE_REQUIRE_DARWIN:-0}"
TARGETS="${SOFTLINE_PACKAGE_TARGETS:-${SOFTLINE_RELEASE_TARGETS}}"
BUILT_TARGETS=""
SKIPPED_TARGETS=""

for target in ${TARGETS}; do
  case " ${SOFTLINE_RELEASE_TARGETS} " in
    *" ${target} "*) ;;
    *) echo "ERROR: unknown package target: ${target}" >&2; exit 2 ;;
  esac
done
mkdir -p "${ROOT_DIR}/build/package-state"
: > "${ROOT_DIR}/build/package-state/targets"
if [ "${REQUIRE_DARWIN}" = "1" ]; then
  case " ${TARGETS} " in
    *" arm64-apple-darwin "*) ;;
    *)
      echo "ERROR: Darwin artifact is required for make release" >&2
      exit 1
      ;;
  esac
  "$ROOT_DIR/scripts/cpkt-toolchains.sh" ensure arm64-apple-darwin >/dev/null
fi

mkdir -p "${DIST_DIR}"

for target in ${TARGETS}; do
  preset="${target}-release"
  build_dir="${ROOT_DIR}/build/${preset}"
  echo "Building ${target}..."
  if [ "${target}" = "arm64-apple-darwin" ] && [ "${REQUIRE_DARWIN}" = "0" ]; then
    description=$("${ROOT_DIR}/scripts/cpkt-toolchains.sh" discover "$target")
    # An installed SDK may still need the build-owned host MIG collection.
    if ! grep -q '^root=' <<< "$description"; then
      echo "SKIP: ${target}: optional osxcross toolchain unavailable"
      SKIPPED_TARGETS="${SKIPPED_TARGETS} ${target}"
      continue
    fi
  fi
  if [[ "$target" != arm64-apple-darwin || "$REQUIRE_DARWIN" != 1 ]]; then
    "$ROOT_DIR/scripts/cpkt-toolchains.sh" ensure "$target" >/dev/null
  fi
  configure=(cmake --preset "$preset")
  if [[ "$target" == x86_64-linux-gnu ]]; then
    configure=("$ROOT_DIR/scripts/terminal-test-env.sh" "${configure[@]}")
  fi
  if ! (cd "${ROOT_DIR}"; "${configure[@]}"); then
    echo "ERROR: ${target} failed to configure using its lifecycle toolchain"
    exit 1
  fi
  if ! (cd "${ROOT_DIR}"; cmake --build --preset "${preset}"); then
    echo "ERROR: ${target} failed to build"
    exit 1
  fi
  if [ "${target}" = "x86_64-linux-gnu" ]; then
    if ! ctest --test-dir "${build_dir}" \
      --stop-on-failure --output-on-failure --no-tests=error; then
      echo "ERROR: ${target} release tests failed"
      exit 1
    fi
  fi
  "$ROOT_DIR/scripts/package-sdk.sh" "$build_dir" "$target" "$DIST_DIR"
  BUILT_TARGETS="${BUILT_TARGETS} ${target}"
  printf '%s\n' "${target}" >> "${ROOT_DIR}/build/package-state/targets"
done

if [ -z "${BUILT_TARGETS}" ]; then
  echo "ERROR: no release targets were packaged"
  exit 1
fi

if [ "${REQUIRE_DARWIN}" = "1" ] &&
   ! printf '%s\n' "${BUILT_TARGETS}" | grep -Eq '(^|[[:space:]])arm64-apple-darwin([[:space:]]|$)'; then
  echo "ERROR: Darwin artifact is required for make release" >&2
  exit 1
fi

echo "Packages created in ${DIST_DIR}/"
ls -la "${DIST_DIR}/"
echo "Built targets:${BUILT_TARGETS}"
if [ -n "${SKIPPED_TARGETS}" ]; then
  echo "Skipped targets:${SKIPPED_TARGETS}"
fi
