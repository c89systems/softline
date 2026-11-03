#!/usr/bin/env bash
case ${BASH_SOURCE[0]} in
  */*) source "${BASH_SOURCE[0]%/*}/require-host-bash.sh" ;;
  *) source ./require-host-bash.sh ;;
esac || exit $?
set -euo pipefail
source "${BASH_SOURCE[0]%/*}/require-workspace.sh" || exit $?

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd -P)"
PRODUCER_DIR="${ROOT_DIR}/build/consumer-producer"
version_only=0
producer_args=()
while (($#)); do
  case "$1" in
    --version-override-only) version_only=1; shift ;;
    --producer)
      [[ $# -ge 2 && -n $2 ]] || { echo 'ERROR: --producer needs a prepared build directory' >&2; exit 2; }
      PRODUCER_DIR=$2
      SOFTLINE_GENERATED_PATH="$PRODUCER_DIR" source "${BASH_SOURCE[0]%/*}/require-workspace.sh" || exit $?
      [[ -f "$PRODUCER_DIR/CMakeCache.txt" ]] || { echo 'ERROR: prepare the selected consumer producer first' >&2; exit 1; }
      producer_args=(--producer "$PRODUCER_DIR")
      shift 2
      ;;
    *) echo 'Usage: package-consumer-smoke.sh [--producer prepared-build-dir] [--version-override-only]' >&2; exit 2 ;;
  esac
done
mkdir -p "${ROOT_DIR}/build"
# Exercise ordinary path spaces in both CMake and pkg-config consumers.
TMP_DIR="$(mktemp -d "${ROOT_DIR}/build/consumer with spaces.XXXXXX")"
VERSION="$(bash "${ROOT_DIR}/scripts/release_version.sh")"
SOFTLINE_ABI_VERSION="${SOFTLINE_ABI_VERSION:-4}"
BOOTLIN_TOOLCHAIN="${ROOT_DIR}/cmake/toolchains/bootlin-linux.cmake"
BOOTLIN_TARGET=""
BOOTLIN_TOOLCHAIN_ARG=""
BOOTLIN_TARGET_ARG=""
trap 'rm -rf "${TMP_DIR}"' EXIT
mkdir -p "$TMP_DIR/bin"
cat > "$TMP_DIR/bin/cpkt" <<'EOF'
#!/bin/sh
printf 'unexpected cpkt invocation\n' >> "$SOFTLINE_UNEXPECTED_CPKT_CALLS"
exit 99
EOF
chmod +x "$TMP_DIR/bin/cpkt"
export PATH="$TMP_DIR/bin:$PATH" SOFTLINE_UNEXPECTED_CPKT_CALLS="$TMP_DIR/cpkt-calls"

BOOTLIN_TARGET="$("${ROOT_DIR}/scripts/native-linux-target.sh")"
toolchain_env="$("${ROOT_DIR}/scripts/cpkt-toolchains.sh" env "${BOOTLIN_TARGET}")" || exit $?
eval "$toolchain_env"
BOOTLIN_TOOLCHAIN_ARG="-DCMAKE_TOOLCHAIN_FILE=${BOOTLIN_TOOLCHAIN}"
BOOTLIN_TARGET_ARG="-DSL_TARGET_ID=${BOOTLIN_TARGET}"
CONSUMER_CC="${CC}"
READELF="${CPKT_TOOLCHAIN_READELF:?selected collection did not report readelf}"

APP_DIR="${TMP_DIR}/consumer"

mkdir -p "${APP_DIR}"
cat > "${APP_DIR}/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.24)
project(softline_consumer_smoke LANGUAGES C)

set(CMAKE_DISABLE_FIND_PACKAGE_cpkt TRUE)
find_package(softline REQUIRED CONFIG)

add_executable(consumer main.c)
target_compile_options(consumer PRIVATE -std=c89 -Wall -Wextra -Wpedantic -pedantic-errors -Werror)
target_link_libraries(consumer PRIVATE softline::softline)
include("${SOFTLINE_RUNTIME_HELPER}")
softline_local_runtime(consumer)
EOF

cat > "${APP_DIR}/main.c" <<'EOF'
#include "softline/softline.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct stream_state {
  const char *text;
  int done;
};

static int one_chunk(sl_t *sl, void *userdata, const char **chunk,
                     size_t *len) {
  struct stream_state *state;
  (void)sl;
  state = (struct stream_state *)userdata;
  if (!state || state->done) {
    *chunk = NULL;
    *len = 0;
    return SL_OK;
  }
  state->done = 1;
  *chunk = state->text;
  *len = strlen(state->text);
  return SL_OK;
}

int main(void) {
  int input_pipe[2];
  int output_pipe[2];
  sl_config_t cfg;
  sl_t *sl;
  char *line;
  struct stream_state stream;
  sl_prompt_id_t held, ordinary;
  sl_prompt_queue_entry_info_t info;
  sl_prompt_source_t source;
  int stopped;

  if (pipe(input_pipe) != 0 || pipe(output_pipe) != 0)
    return 2;
  if (write(input_pipe[1], "hello\n", 6) != 6)
    return 3;
  close(input_pipe[1]);

  sl_config_init(&cfg);
  cfg.input_fd = input_pipe[0];
  cfg.output_fd = output_pipe[1];
  cfg.prompt_queue = 1;
  sl = sl_create_with_config(&cfg);
  if (!sl)
    return 4;

  if (sl->last_readline_status(sl) != SL_READLINE_NONE)
    return 5;
  if (sl_history_add(sl, "history") != SL_OK)
    return 6;
  if (sl_set_buffer(sl, "draft") != SL_OK)
    return 7;
  if (strcmp(sl_buffer(sl), "draft") != 0)
    return 8;
  if (sl_set_cursor(sl, 2) != SL_OK || sl_cursor(sl) != 2)
    return 9;
  stream.text = "x";
  stream.done = 0;
  if (sl->print_above(sl, one_chunk, &stream) != SL_OK)
    return 10;

  line = sl->readline(sl, "p> ");
  if (!line)
    return 11;
  if (strcmp(line, "hello") != 0)
    return 12;
  if (sl_last_readline_status(sl) != SL_READLINE_SUBMITTED)
    return 13;

  sl_free_string(sl, line);
  if (sl_prompt_queue_append(sl, "approval", 1, &held) != SL_OK ||
      sl_prompt_queue_append(sl, "ordinary", 0, &ordinary) != SL_OK ||
      sl_prompt_queue_info(sl, 0, &info) != SL_OK ||
      info.id != held || !info.held)
    return 14;
  line = sl_next_prompt(sl, NULL, &source);
  if (!line || strcmp(line, "ordinary") || sl_last_prompt_id(sl) != ordinary)
    return 15;
  sl_free_string(sl, line);
  if (sl_prompt_queue_approve(sl, held) != SL_OK)
    return 16;
  line = sl_next_prompt(sl, NULL, &source);
  if (!line || strcmp(line, "approval") || sl_last_prompt_id(sl) != held)
    return 17;
  sl_free_string(sl, line);
  if (sl_prompt_queue_append(sl, "rejected", 1, &held) != SL_OK ||
      sl_prompt_queue_reject(sl, held) != SL_OK || sl_prompt_queue_count(sl))
    return 18;
  if (sl_inject_queued_prompt(sl, "queued injection", &held) != SL_OK ||
      sl_inject_steer_prompt(sl, "steering injection", &ordinary) != SL_OK ||
      sl_set_auto_approval(sl, 1) != SL_OK ||
      sl_prompt_queue_info(sl, 0, &info) != SL_OK || info.held || info.id != held ||
      sl_prompt_queue_info(sl, 1, &info) != SL_OK || info.held ||
      info.id != ordinary || info.mode != SL_PROMPT_QUEUE_MODE_STEER ||
      sl_prompt_queue_clear(sl) != SL_OK)
    return 19;
  if (!sl->set_prompt_queue_policy || !sl->prompt_queue_set_executable ||
      !sl->get_prompt_queue_stopped || !sl->prompt_queue_resume ||
      sl_set_prompt_queue_policy(sl, NULL, NULL) != SL_OK ||
      sl_get_prompt_queue_stopped(sl, &stopped) != SL_OK || stopped ||
      sl_prompt_queue_resume(sl) != SL_OK ||
      sl_prompt_queue_append(sl, "boundary", 0, &ordinary) != SL_OK ||
      sl_prompt_queue_set_executable(sl, ordinary, 0) != SL_OK ||
      sl_prompt_queue_info(sl, 0, &info) != SL_OK || info.eligible ||
      info.policy.executable ||
      sl_prompt_queue_take(sl, 0, &line) != SL_ERROR_INVALID || line ||
      sl->prompt_queue_set_executable(sl, ordinary, 1) != SL_OK ||
      sl_prompt_queue_take(sl, 0, &line) != SL_OK ||
      strcmp(line, "boundary") != 0)
    return 20;
  sl_free_string(sl, line);
  if (sl->output_stream_begin(sl) != SL_OK ||
      sl->output_stream_reconcile(sl) != SL_OK ||
      sl_output_stream_reconcile(sl) != SL_OK ||
      sl->output_stream_end(sl) != SL_OK)
    return 13;
  sl_destroy(sl);
  close(input_pipe[0]);
  close(output_pipe[0]);
  close(output_pipe[1]);
  return 0;
}
EOF

install_disabled_smoke() {
  build_dir="${PRODUCER_DIR}"
  install_dir="${TMP_DIR}/install-disabled"

  cmake -S "${ROOT_DIR}" -B "${build_dir}" \
    -G Ninja \
    ${BOOTLIN_TOOLCHAIN_ARG:+"${BOOTLIN_TOOLCHAIN_ARG}"} \
    ${BOOTLIN_TARGET_ARG:+"${BOOTLIN_TARGET_ARG}"} \
    -DSL_BUILD_EXAMPLES=OFF \
    -DSL_BUILD_TESTS=OFF \
    -DCMAKE_BUILD_TYPE=Release \
    -DSL_BUILD_STATIC=ON -DSL_BUILD_SHARED=ON \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DSL_INSTALL=OFF \
    -DCMAKE_INSTALL_PREFIX="${install_dir}"
  cmake --build "${build_dir}"
  cmake --install "${build_dir}" --prefix "${install_dir}"
  if [ -n "$(find "${install_dir}" -type f -print 2>/dev/null)" ]; then
    echo "ERROR: SL_INSTALL=OFF installed files"
    exit 1
  fi

  echo "  install-disabled: OK"
}

verify_shared_abi() {
  libdir="$1"
  shared_lib="${libdir}/libsoftline.so"
  abi_lib="${libdir}/libsoftline.so.${SOFTLINE_ABI_VERSION}"
  soname=""

  if [ ! -e "${shared_lib}" ]; then
    echo "ERROR: shared install missing libsoftline.so"
    exit 1
  fi
  if [ ! -e "${abi_lib}" ]; then
    echo "ERROR: shared install missing ABI symlink ${abi_lib}"
    exit 1
  fi
  if { [ -z "${READELF:-}" ] || [ ! -x "${READELF}" ]; }; then
    echo "ERROR: lifecycle readelf is required for package consumer ABI verification" >&2
    exit 1
  fi
  soname="$("${READELF}" -d "${shared_lib}" |
    sed -n 's/.*Library soname: \[\(.*\)\].*/\1/p')"
  if [ "${soname}" != "libsoftline.so.${SOFTLINE_ABI_VERSION}" ]; then
    echo "ERROR: shared install SONAME ${soname} does not match ABI ${SOFTLINE_ABI_VERSION}"
    exit 1
  fi
}

smoke_mode() {
  mode="$1"
  build_static="$2"
  build_shared="$3"
  install_libdir_arg="${4:-}"
  install_dir="${TMP_DIR}/install-${mode}"
  build_dir="${PRODUCER_DIR}"
  app_build_dir="${TMP_DIR}/consumer-build-${mode}"
  pkg_consumer="${TMP_DIR}/pkg-config-consumer-${mode}"
  configure_libdir_arg="-DCMAKE_INSTALL_LIBDIR=${install_libdir_arg:-lib}"
  install_libdir=""

  if [ -n "${install_libdir_arg}" ]; then
    configure_libdir_arg="-DCMAKE_INSTALL_LIBDIR=${install_libdir_arg}"
  fi

  cmake -S "${ROOT_DIR}" -B "${build_dir}" \
    -G Ninja \
    ${BOOTLIN_TOOLCHAIN_ARG:+"${BOOTLIN_TOOLCHAIN_ARG}"} \
    ${BOOTLIN_TARGET_ARG:+"${BOOTLIN_TARGET_ARG}"} \
    -DSL_BUILD_EXAMPLES=OFF \
    -DSL_BUILD_TESTS=OFF \
    -DCMAKE_BUILD_TYPE=Release \
    -DSL_BUILD_STATIC="${build_static}" \
    -DSL_BUILD_SHARED="${build_shared}" \
    -DSL_INSTALL=ON \
    -DCMAKE_INSTALL_PREFIX="${install_dir}" \
    ${configure_libdir_arg}
  cmake --build "${build_dir}"
  cmake --install "${build_dir}"

  if [ -d "${install_dir}/lib/pkgconfig" ]; then
    install_libdir="${install_dir}/lib"
  elif [ -d "${install_dir}/lib64/pkgconfig" ]; then
    install_libdir="${install_dir}/lib64"
  else
    echo "ERROR: install tree missing lib/pkgconfig or lib64/pkgconfig"
    exit 1
  fi
  if [ "${build_shared}" = "ON" ]; then
    verify_shared_abi "${install_libdir}"
  fi

  cmake -S "${APP_DIR}" -B "${app_build_dir}" \
    -G Ninja \
    ${BOOTLIN_TOOLCHAIN_ARG:+"${BOOTLIN_TOOLCHAIN_ARG}"} \
    ${BOOTLIN_TARGET_ARG:+"${BOOTLIN_TARGET_ARG}"} \
    -DCMAKE_PREFIX_PATH="${install_dir}" \
    -DSOFTLINE_RUNTIME_HELPER="${ROOT_DIR}/cmake/softline_local_runtime.cmake" \
    -Dsoftline_DIR="${install_libdir}/cmake/softline"
  cmake --build "${app_build_dir}"
  "${app_build_dir}/consumer"

  if ! command -v pkg-config >/dev/null 2>&1; then
    echo "ERROR: pkg-config is required for package consumer smoke"
    exit 1
  fi
  PKG_CONFIG_PATH="${install_libdir}/pkgconfig"
  PKG_CONFIG_LIBDIR="${install_libdir}/pkgconfig"
  export PKG_CONFIG_PATH PKG_CONFIG_LIBDIR
  pkg-config --exists softline
  test "$(pkg-config --modversion softline)" = "${VERSION}"
  # pkg-config emits shell-quoted arguments from this verified SDK.
  pkg_flags=$(pkg-config --cflags --libs softline)
  eval "pkg_args=($pkg_flags)"
  "${CONSUMER_CC}" -std=c89 -Wall -Wextra -Wpedantic -pedantic-errors -Werror \
    @"${app_build_dir}/consumer-runtime.flags" \
    "${APP_DIR}/main.c" "${pkg_args[@]}" -Wl,-rpath,"${install_libdir}" \
    -o "${pkg_consumer}"
  "${pkg_consumer}"
  test ! -e "$SOFTLINE_UNEXPECTED_CPKT_CALLS"

  echo "  ${mode}: OK"
}

install_disabled_smoke
smoke_mode both ON ON
if [ "$version_only" = 1 ]; then
  echo "Installed package consumer smoke test passed."
  exit 0
fi
smoke_mode static-only ON OFF
smoke_mode shared-only OFF ON
smoke_mode both-lib64 ON ON lib64
SL_VERSION_OVERRIDE=v1.2.3 bash "$0" --version-override-only "${producer_args[@]}"

echo "Installed package consumer smoke test passed."
