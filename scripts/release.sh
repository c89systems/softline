#!/usr/bin/env bash
case ${BASH_SOURCE[0]} in
  */*) source "${BASH_SOURCE[0]%/*}/require-host-bash.sh" ;;
  *) source ./require-host-bash.sh ;;
esac || exit $?
set -euo pipefail
SOFTLINE_SHARED_CACHES_ONLY=1 source "${BASH_SOURCE[0]%/*}/require-workspace.sh" || exit $?
ROOT_DIR=$(cd "${BASH_SOURCE[0]%/*}/.." && pwd)
mode=${1:-run}
case "$mode" in preflight|run) ;; *) echo 'Usage: release.sh [preflight|run]' >&2; exit 2 ;; esac
cd "$ROOT_DIR"
"$ROOT_DIR/scripts/lifecycle-version-contract.sh"
if [[ "$mode" == preflight ]]; then
  [[ -z ${SL_VERSION_OVERRIDE:-} && $("$ROOT_DIR/scripts/release_version.sh") != 0.0.0 ]] || {
    echo 'ERROR: make release is reserved for the final exact lightweight tagged release; use make prerelease for incremental preparation' >&2; exit 1;
  }
  [[ -z $(git status --porcelain) ]] || { echo 'ERROR: final tagged release requires a clean worktree' >&2; exit 1; }
  [[ -z ${SOFTLINE_PACKAGE_TARGETS:-} && -z ${SOFTLINE_DIST_DIR:-} && -z ${SOFTLINE_FORCE_TESTS:-} &&
     -z ${SOFTLINE_RELEASE_TARGETS:-} && -z ${SOFTLINE_CHECKSUM_FILE:-} && -z ${SL_VERSION_ROOT:-} ]] || {
    echo 'ERROR: final release refuses partial targets, alternate dist roots or evidence selectors' >&2; exit 1;
  }
  for tool in cmake ninja bash clang-format clangd pkg-config luarocks valgrind tar gzip zip unzip; do
    command -v "$tool" >/dev/null || { echo "ERROR: required release tool missing: $tool" >&2; exit 1; }
  done
  # Prepare pinned collections before the release removes generated outputs.
  source "$ROOT_DIR/scripts/release-targets.sh"
  "$ROOT_DIR/scripts/cpkt-toolchains.sh" ensure arm64-apple-darwin >/dev/null
  for target in $SOFTLINE_RELEASE_TARGETS; do
    [[ "$target" != arm64-apple-darwin ]] || continue
    "$ROOT_DIR/scripts/cpkt-toolchains.sh" ensure "$target" >/dev/null
  done
  "$ROOT_DIR/scripts/cpkt-aflpp.sh" ensure
  exit 0
fi
# make release has already checked the tag and emptied local generated state.
# Ordinary host/hardening proof stays in the checkout. Delivered source owns
# the native Release lane; other target configurations retain their own graph.
make -C "$ROOT_DIR" prerelease-runtime-checks
"$ROOT_DIR/scripts/package-source.sh"
version=$("$ROOT_DIR/scripts/release_version.sh")
source=$("$ROOT_DIR/scripts/extract-release-source.sh" \
  "$ROOT_DIR/dist/softline-$version.tar.gz" "$ROOT_DIR/build/release-source")
export SOFTLINE_DIST_DIR="$ROOT_DIR/dist"
SOFTLINE_PACKAGE_TARGETS=x86_64-linux-gnu SOFTLINE_REQUIRE_DARWIN=0 "$source/scripts/package.sh"
"$source/scripts/package-consumer-smoke.sh" --producer "$source/build/x86_64-linux-gnu-release"
source "$ROOT_DIR/scripts/release-targets.sh"
cross_targets=
for target in $SOFTLINE_RELEASE_TARGETS; do
  [[ "$target" == x86_64-linux-gnu ]] || cross_targets+=" $target"
done
SOFTLINE_PACKAGE_TARGETS="$cross_targets" SOFTLINE_REQUIRE_DARWIN=1 "$ROOT_DIR/scripts/package.sh"
"$ROOT_DIR/scripts/release_lua_artifacts.sh"
"$ROOT_DIR/scripts/package-checksums.sh" all
"$ROOT_DIR/scripts/package-verify.sh"
echo "Final clean tagged release verified from delivered source: $version"
