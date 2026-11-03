#!/usr/bin/env bash
case ${BASH_SOURCE[0]} in
  */*) source "${BASH_SOURCE[0]%/*}/require-host-bash.sh" ;;
  *) source ./require-host-bash.sh ;;
esac || exit $?
set -euo pipefail
source "${BASH_SOURCE[0]%/*}/require-workspace.sh" || exit $?

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd -P)"
DIST_DIR="${SOFTLINE_DIST_DIR:-${ROOT_DIR}/dist}"
# Keep caller-relative inputs stable when checksum verification changes cwd.
[[ "$DIST_DIR" == /* ]] || DIST_DIR="$PWD/$DIST_DIR"
mkdir -p "${ROOT_DIR}/build"
# Relocated consumer proof also exercises ordinary SDK path spaces.
TMP_DIR="$(mktemp -d "${ROOT_DIR}/build/package verify.XXXXXX")"
trap 'rm -rf "${TMP_DIR}"' EXIT

if [ ! -d "${DIST_DIR}" ] || [ -z "$(ls -A "${DIST_DIR}" 2>/dev/null)" ]; then
  echo "No packages to verify in ${DIST_DIR}/"
  exit 1
fi

VERSION="$(bash "${ROOT_DIR}/scripts/release_version.sh")"
CHECKSUM_FILE="${SOFTLINE_CHECKSUM_FILE:-${DIST_DIR}/softline-${VERSION}-CHECKSUMS}"
[[ "$CHECKSUM_FILE" == /* ]] || CHECKSUM_FILE="$PWD/$CHECKSUM_FILE"
SCOPED=0
[[ -z ${SOFTLINE_CHECKSUM_FILE:-} ]] || SCOPED=1
SOFTLINE_ABI_VERSION="${SOFTLINE_ABI_VERSION:-4}"

if [ ! -f "${CHECKSUM_FILE}" ]; then
  echo "ERROR: missing checksum manifest ${CHECKSUM_FILE}"
  exit 1
fi

if [ -e "${DIST_DIR}/SHA256SUMS" ]; then
  echo "ERROR: deprecated checksum manifest present: ${DIST_DIR}/SHA256SUMS"
  exit 1
fi

source "${ROOT_DIR}/scripts/release-targets.sh"
required=("softline-${VERSION}.tar.gz" "softline-lua-${VERSION}.tar.gz"
  "softline-${VERSION}-1.rockspec" "softline-${VERSION}-1.src.rock")
for target in $SOFTLINE_RELEASE_TARGETS; do required+=("softline-${VERSION}-${target}.tar.gz"); done
if [ "$SCOPED" = 0 ]; then
for name in "${required[@]}"; do
  [[ -f "$DIST_DIR/$name" && ! -L "$DIST_DIR/$name" ]] || {
    echo "ERROR: missing or unsafe required release payload: $name" >&2; exit 1;
  }
  awk '{print $NF}' "$CHECKSUM_FILE" | grep -Fxq "$name" || {
    echo "ERROR: required release payload absent from manifest: $name" >&2; exit 1;
  }
done
for checksum in "${DIST_DIR}"/softline-*-CHECKSUMS; do
  [ -f "${checksum}" ] || continue
  if [ "${checksum}" != "${CHECKSUM_FILE}" ]; then
    echo "ERROR: stale checksum manifest present: ${checksum}"
    exit 1
  fi
done
fi

declare -A inventory=()
while read -r checksum filename extra || [[ -n "$checksum$filename$extra" ]]; do
  [[ "$checksum" =~ ^[0-9a-fA-F]{64}$ && -n "$filename" && -z "$extra" ]] || {
    echo 'ERROR: invalid checksum entry' >&2; exit 1;
  }
  found=0
  for expected in "${required[@]}"; do [[ "$filename" != "$expected" ]] || found=1; done
  ((found)) || { echo "ERROR: unexpected checksum artifact: $filename" >&2; exit 1; }
  [[ -z ${inventory[$filename]:-} ]] || {
    echo "ERROR: duplicate checksum artifact: $filename" >&2; exit 1;
  }
  [[ -f "$DIST_DIR/$filename" && ! -L "$DIST_DIR/$filename" ]] || {
    echo "ERROR: missing or unsafe checksum artifact: $filename" >&2; exit 1;
  }
  inventory[$filename]=1
done < "$CHECKSUM_FILE"

target_from_basename() {
  basename="$1"
  echo "${basename#softline-${VERSION}-}"
}

load_tools() {
  target="$1"
  build_dir="${ROOT_DIR}/build/${target}-release"
  tool_env=""
  TARGET_ID=""
  TARGET_OS=""
  CC=""
  LD=""
  LINKER=""
  READELF=""
  NM=""
  OTOOL=""
  INSTALL_NAME_TOOL=""
  STRIP=""
  if tool_env="$("${ROOT_DIR}/scripts/cpkt-toolchains.sh" env "${target}" 2>/dev/null)"; then
    eval "${tool_env}"
    TARGET_ID="${target}"
    case "${target}" in
      *-apple-darwin) TARGET_OS="darwin" ;;
      *-linux-*) TARGET_OS="linux" ;;
      *) TARGET_OS="unknown" ;;
    esac
    LINKER="${LD:-${CPKT_TOOLCHAIN_LD:-}}"
    READELF="${CPKT_TOOLCHAIN_READELF:-}"
    OTOOL="${CPKT_TOOLCHAIN_OTOOL:-}"
    INSTALL_NAME_TOOL="${CPKT_TOOLCHAIN_INSTALL_NAME_TOOL:-}"
    STRIP="${CPKT_TOOLCHAIN_STRIP:-}"
    NM="${CPKT_TOOLCHAIN_NM:-${NM:-}}"
    return
  fi
  tools="$("${ROOT_DIR}/scripts/discover_target_tools.sh" "${build_dir}" "${target}")"
  eval "${tools}"
}

fail_local_path() {
  artifact="$1"
  file="$2"
  echo "ERROR: ${artifact} contains local path in ${file}"
  exit 1
}

scan_for_local_paths() {
  artifact="$1"
  root="$2"
  if grep -R -a -F "${ROOT_DIR}" "${root}" >/dev/null 2>&1; then
    fail_local_path "${artifact}" "${root}"
  fi
  if grep -R -a -F "${HOME}" "${root}" >/dev/null 2>&1; then
    fail_local_path "${artifact}" "${root}"
  fi
  if grep -R -a -F "file://${HOME}" "${root}" >/dev/null 2>&1; then
    fail_local_path "${artifact}" "${root}"
  fi
}

verify_darwin_metadata() {
  artifact="$1"
  root="$2"
  if [ -z "${OTOOL:-}" ]; then
    echo "ERROR: ${artifact}: external-tool-unavailable: target-correct otool required for Mach-O verification"
    exit 1
  fi
  find "${root}" -type f \( -name '*.dylib' -o -name '*.so' \) | while IFS= read -r file; do
    id="$("${OTOOL}" -D "${file}" 2>/dev/null | sed -n '2p' || true)"
    case "${file}" in
      *.dylib)
        case "${id}" in
            "@rpath/libsoftline.${SOFTLINE_ABI_VERSION}.dylib") ;;
          "")
            echo "ERROR: ${artifact}: ${file} missing LC_ID_DYLIB"
            exit 1
            ;;
          *)
            echo "ERROR: ${artifact}: ${file} has an incorrect ABI install name ${id}"
            exit 1
            ;;
        esac
        ;;
    esac

    "${OTOOL}" -L "${file}" 2>/dev/null | sed '1d' | while IFS= read -r dep_line; do
      dep="$(printf '%s\n' "${dep_line}" | awk '{print $1}')"
      case "${dep}" in
        @rpath/*|@loader_path/*|@executable_path/*) ;;
        /usr/lib/*|/System/Library/*) ;;
        /*)
          echo "ERROR: ${artifact}: ${file} has non-system absolute Mach-O dependency ${dep}"
          exit 1
          ;;
      esac
    done

    "${OTOOL}" -l "${file}" 2>/dev/null |
      awk '
        $1 == "cmd" && $2 == "LC_RPATH" { in_rpath = 1; next }
        in_rpath && $1 == "path" { print $2; in_rpath = 0 }
      ' | while IFS= read -r rpath; do
        case "${rpath}" in
          @loader_path*|@executable_path*) ;;
          "")
            ;;
          *)
            echo "ERROR: ${artifact}: ${file} has non-relocatable Darwin rpath ${rpath}"
            exit 1
            ;;
        esac
      done
  done
}

verify_dynamic_exports() {
  artifact="$1"
  target="$2"
  pkg_lib_dir="$3"
  shared_library=""

  if [ -z "${NM:-}" ] || [ ! -x "${NM}" ]; then
    echo "ERROR: ${artifact}: target-correct nm is required for dynamic export verification" >&2
    exit 1
  fi
  case "${target}" in
    *-linux-*)
      shared_library="$(find "${pkg_lib_dir}" -type f -name 'libsoftline.so.*' | head -1)"
      ;;
    *-apple-darwin)
      shared_library="$(find "${pkg_lib_dir}" -type f -name 'libsoftline.*.dylib' | head -1)"
      ;;
    *)
      shared_library=""
      ;;
  esac
  if [ -z "${shared_library}" ]; then
    echo "ERROR: ${artifact}: shared libsoftline payload missing" >&2
    exit 1
  fi
  case "${target}" in
    *-linux-*) format=elf ;;
    *-apple-darwin) format=macho ;;
    *)
      echo "ERROR: ${artifact}: unknown target for dynamic export verification: ${target}" >&2
      exit 1
      ;;
  esac
  if ! bash "$ROOT_DIR/scripts/verify-public-exports.sh" "$NM" "$shared_library" \
      "$ROOT_DIR/cmake/softline.exports" "$format"; then
    echo "ERROR: ${artifact}: libsoftline dynamic exports differ from cmake/softline.exports" >&2
    exit 1
  fi
}

verify_extracted_consumer() {
  target="$1"
  pkg_root="$2"
  pkg_lib_dir="$3"
  consumer_dir="${TMP_DIR}/consumer-${target}"
  cmake_toolchain="${ROOT_DIR}/cmake/toolchains/bootlin-linux.cmake"

  case "${target}" in
    arm64-apple-darwin)
      cmake_toolchain="${ROOT_DIR}/cmake/toolchains/osxcross-darwin.cmake"
      ;;
  esac

  rm -rf "${consumer_dir}"
  mkdir -p "${consumer_dir}"
  cat > "${consumer_dir}/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.24)
project(softline_extracted_consumer LANGUAGES C)
find_package(softline ${SOFTLINE_EXPECTED_VERSION} EXACT REQUIRED CONFIG)
add_executable(softline_extracted_consumer main.c)
target_link_libraries(softline_extracted_consumer PRIVATE softline::softline)
add_executable(softline_extracted_static main.c)
target_link_libraries(softline_extracted_static PRIVATE softline::softline_static)
target_compile_options(softline_extracted_consumer PRIVATE
  -std=c89 -Wall -Wextra -Wpedantic -pedantic-errors -Werror)
target_compile_options(softline_extracted_static PRIVATE
  -std=c89 -Wall -Wextra -Wpedantic -pedantic-errors -Werror)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(softline_extracted_consumer PRIVATE -Wl,--fatal-warnings)
  target_link_options(softline_extracted_static PRIVATE -static -Wl,--fatal-warnings)
endif()
include("${SOFTLINE_RUNTIME_HELPER}")
softline_local_runtime(softline_extracted_consumer)
EOF
  cat > "${consumer_dir}/main.c" <<'EOF'
#include "softline/softline.h"

int main(void) {
  sl_config_t config;
  sl_config_init(&config);
  return config.input_fd == 0 ? 0 : 1;
}
EOF

  set -- "-DCMAKE_TOOLCHAIN_FILE=${cmake_toolchain}"
  case "${target}" in
    arm64-apple-darwin) ;;
    *) set -- "$@" "-DSL_TARGET_ID=${target}" ;;
  esac
  cmake -S "${consumer_dir}" -B "${consumer_dir}/cmake-build" -G Ninja \
    "$@" \
    -DCMAKE_PREFIX_PATH="${pkg_root}" \
    -DSOFTLINE_EXPECTED_VERSION="${VERSION}" \
    -DSOFTLINE_RUNTIME_HELPER="${ROOT_DIR}/cmake/softline_local_runtime.cmake" \
    -Dsoftline_DIR="${pkg_lib_dir}/cmake/softline"
  cmake --build "${consumer_dir}/cmake-build"

  PKG_CONFIG_PATH="${pkg_lib_dir}/pkgconfig"
  export PKG_CONFIG_PATH
  pkg_flags=$(pkg-config --cflags --libs softline)
  eval "shared_args=($pkg_flags)"
  pkg_flags=$(pkg-config --cflags --static --libs softline)
  eval "static_args=($pkg_flags)"
  if [ "${target}" = "arm64-apple-darwin" ]; then
    if [ -z "${LINKER:-}" ]; then
      echo "ERROR: ${target}: target linker unavailable for pkg-config consumer" >&2
      exit 1
    fi
    PATH="$(dirname "${LINKER}"):${PATH}" "${CC}" --ld-path="${LINKER}" \
      -std=c89 -Wall -Wextra -Wpedantic -pedantic-errors -Werror \
      "${consumer_dir}/main.c" "${shared_args[@]}" -o "${consumer_dir}/pkg-config-consumer"
  else
    "${CC}" -Wl,--fatal-warnings -std=c89 -Wall -Wextra -Wpedantic -pedantic-errors -Werror \
      @"${consumer_dir}/cmake-build/softline_extracted_consumer-runtime.flags" \
      "${consumer_dir}/main.c" "${shared_args[@]}" -Wl,-rpath,"${pkg_lib_dir}" -o "${consumer_dir}/pkg-config-consumer"
  fi

  if [ "${target}" = arm64-apple-darwin ]; then
    PATH="$(dirname "${LINKER}"):${PATH}" "${CC}" --ld-path="${LINKER}" -std=c89 -Wall -Wextra -Wpedantic -pedantic-errors -Werror \
      "${consumer_dir}/main.c" "${static_args[@]}" -o "${consumer_dir}/static-consumer"
  else
    "${CC}" -Wl,--fatal-warnings -static -std=c89 -Wall -Wextra -Wpedantic -pedantic-errors -Werror \
      "${consumer_dir}/main.c" "${static_args[@]}" -o "${consumer_dir}/static-consumer"
  fi
  if [ "${target}" = "x86_64-linux-gnu" ] || [ "${target}" = "x86_64-linux-musl" ]; then
    "${consumer_dir}/cmake-build/softline_extracted_consumer"
    "${consumer_dir}/cmake-build/softline_extracted_static"
    "${consumer_dir}/pkg-config-consumer"
    "${consumer_dir}/static-consumer"
  fi
}

echo "Verifying checksums..."
cd "${DIST_DIR}"
if command -v sha256sum >/dev/null 2>&1; then
  sha256sum -c "${CHECKSUM_FILE}"
elif command -v shasum >/dev/null 2>&1; then
  shasum -a 256 -c "${CHECKSUM_FILE}"
else
  echo "ERROR: sha256sum or shasum is required to verify checksums"
  exit 1
fi
cd "${ROOT_DIR}"

if [ "$SCOPED" = 0 ]; then
for artifact in "${DIST_DIR}"/softline-*; do
  [ -f "${artifact}" ] || continue
  name="$(basename "${artifact}")"
  case "${name}" in
    *-CHECKSUMS) continue ;;
    *.tar.gz|*.rockspec|*.src.rock) ;;
    *) continue ;;
  esac
  if ! awk '{print $NF}' "${CHECKSUM_FILE}" | grep -Fx "${name}" >/dev/null; then
    echo "ERROR: release artifact omitted from checksum manifest: ${name}"
    exit 1
  fi
done

fi
echo "Verifying package layouts..."
while IFS= read -r filename; do
  case "$filename" in
    *.tar.gz) ;;
    *) "$ROOT_DIR/scripts/verify-artifact-runtime.sh" "$DIST_DIR/$filename"; continue ;;
  esac
  [[ "$filename" == "${filename##*/}" ]] || { echo 'ERROR: checksum artifact must be a basename' >&2; exit 1; }
  archive="${DIST_DIR}/${filename}"
  [ -f "${archive}" ] || continue
  basename="$(basename "${archive}" .tar.gz)"
  echo "  Checking ${basename}..."
  if [ "${basename}" = "softline-lua-${VERSION}" ]; then
    "$ROOT_DIR/scripts/verify-artifact-runtime.sh" "$archive"
    echo "  ${basename}: Lua source archive, verified by validate-luarocks"
    continue
  fi
  if [ "${basename}" = "softline-${VERSION}" ]; then
    "$ROOT_DIR/scripts/verify-artifact-runtime.sh" "$archive"
    echo "  ${basename}: source archive is verified only by package-source-smoke"
    continue
  fi

  target="$(target_from_basename "${basename}")"
  members=$(tar -tzf "$archive")
  while IFS= read -r member; do
    case "$member" in
      "$basename"|"$basename/"|"$basename/"*) ;;
      *) echo "ERROR: unexpected SDK archive entry: $member" >&2; exit 1 ;;
    esac
  done <<< "$members"
  load_tools "${target}"
  "$ROOT_DIR/scripts/verify-artifact-runtime.sh" "$archive" --readelf "${READELF:-}"

  rm -rf "${TMP_DIR:?}"/*
  tar -xzf "${archive}" -C "${TMP_DIR}"

  pkg_root="${TMP_DIR}/${basename}"
  scan_for_local_paths "${basename}" "${pkg_root}"
  cmake "-DSDK_ROOT=$pkg_root" "-DSDK_VERSION=$VERSION" "-DSDK_ABI=$SOFTLINE_ABI_VERSION" \
    "-DSDK_TARGET=$target" -P "$ROOT_DIR/cmake/softline_verify_sdk.cmake"

  if [ ! -d "${pkg_root}/include" ]; then
    echo "ERROR: ${basename} missing include/"
    exit 1
  fi

  if [ -d "${pkg_root}/lib" ]; then
    pkg_lib_dir="${pkg_root}/lib"
  elif [ -d "${pkg_root}/lib64" ]; then
    pkg_lib_dir="${pkg_root}/lib64"
  else
    echo "ERROR: ${basename} missing lib/ or lib64/"
    exit 1
  fi

  case "${TARGET_OS}" in
    linux) ;;
    darwin) verify_darwin_metadata "${basename}" "${pkg_root}" ;;
    *)
      echo "ERROR: ${basename}: unknown target OS for ${target}"
      exit 1
      ;;
  esac
  verify_dynamic_exports "${basename}" "${target}" "${pkg_lib_dir}"

  pkg_config_file="${pkg_lib_dir}/pkgconfig/softline.pc"
  if [ ! -f "${pkg_config_file}" ]; then
    echo "ERROR: ${basename} missing pkg-config metadata"
    exit 1
  fi

  if grep -F "${ROOT_DIR}" "${pkg_config_file}" >/dev/null; then
    echo "ERROR: ${basename} pkg-config metadata contains local build path"
    exit 1
  fi

  if ! command -v pkg-config >/dev/null 2>&1; then
    echo "ERROR: pkg-config is required to verify package metadata"
    exit 1
  fi
  PKG_CONFIG_PATH="${pkg_lib_dir}/pkgconfig"
  export PKG_CONFIG_PATH
  pkg-config --exists softline
  [[ $(pkg-config --modversion softline) == "$VERSION" ]] || {
    echo "ERROR: ${basename} pkg-config version differs from $VERSION" >&2; exit 1;
  }
  for selection in --cflags-only-I --libs-only-L; do
    pkg_flags=$(pkg-config --keep-system-cflags --keep-system-libs "$selection" softline)
    eval "directory_args=($pkg_flags)"
    [[ ${#directory_args[@]} == 1 ]] || { echo "ERROR: ${basename}: ambiguous SDK directory" >&2; exit 1; }
    case "$(cd "${directory_args[0]:2}" && pwd -P)" in
      "${pkg_root}/"*) ;;
      *) echo "ERROR: ${basename} pkg-config directory does not point inside package" >&2; exit 1 ;;
    esac
  done

  if [ ! -f "${pkg_root}/share/doc/softline/LICENSE" ]; then
    echo "ERROR: ${basename} missing softline license"
    exit 1
  fi

  if [ ! -f "${pkg_root}/share/doc/softline/README.md" ]; then
    echo "ERROR: ${basename} missing softline README"
    exit 1
  fi

  case "${target}" in
    *-linux-*)
      for notice in LICENSE.md LICENSE.BSD-3-Clause AUTHORS provenance.json README.softline.md softline.patch; do
        if ! cmp -s "${ROOT_DIR}/vendor/c-ares/${notice}" \
             "${pkg_root}/share/softline/licenses/c-ares/${notice}"; then
          echo "ERROR: ${basename} missing or mismatched resolver notice ${notice}" >&2
          exit 1
        fi
      done
      "${NM}" -g "${pkg_lib_dir}/libsoftline.a" | awk '
        /sl_cares_ares_getaddrinfo/ { found=1 }
        $NF ~ /^ares_[A-Za-z0-9_]+$/ { bad=1; print "ERROR: unprefixed upstream symbol " $NF > "/dev/stderr" }
        $0 ~ /[[:space:]]U[[:space:]]/ && $NF ~ /^(getaddrinfo|freeaddrinfo|getnameinfo|gethostbyname2?|gethostbyaddr|getservbyname(_r)?|getservbyport(_r)?|dlopen|dlsym)$/ {
          bad=1; print "ERROR: libc resolver/module dependency " $NF > "/dev/stderr"
        }
        END { exit bad || !found }'

      ;;
  esac

  verify_extracted_consumer "${target}" "${pkg_root}" "${pkg_lib_dir}"

  echo "  ${basename}: OK"
done < <(awk '{ print $NF }' "${CHECKSUM_FILE}")

if [ "$SCOPED" = 0 ] && { [ -f "${DIST_DIR}/softline-${VERSION}-1.src.rock" ] ||
   [ -f "${DIST_DIR}/softline-${VERSION}-1.rockspec" ] ||
   [ -f "${DIST_DIR}/softline-lua-${VERSION}.tar.gz" ]; }; then
  "${ROOT_DIR}/scripts/validate_luarocks.sh"
fi

echo "Package verification passed."
