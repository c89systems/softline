#!/usr/bin/env bash
set -euo pipefail
root=$1
mkdir -p "$root/build"
work=$(mktemp -d "$root/build/lua-selection.XXXXXX")
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/scripts" "$work/commands" "$work/build/lua-sdk/lib/pkgconfig"
cp "$root/scripts/"{lua-test.sh,lua-debug.sh,require-host-bash.sh,require-workspace.sh} "$work/scripts/"
cat > "$work/scripts/terminal-test-env.sh" <<'SH'
#!/bin/sh
exec "$@"
SH
cat > "$work/scripts/native-linux-target.sh" <<'SH'
#!/bin/sh
[ "$SELECT_BOOTLIN" = 1 ] || exit 1
echo x86_64-linux-gnu
SH
for tool in render_lua_rockspec.sh lua-env.sh lua-debug-env.sh; do
  printf '#!/bin/sh\nexit 0\n' > "$work/scripts/$tool"
done
cat > "$work/scripts/build-local-lua.sh" <<'SH'
#!/bin/sh
root=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$root/build/${2:-local-lua}"
printf '%s\n' fixture > "$root/build/${2:-local-lua}/lua-source-dir.txt"
SH
printf '#!/bin/sh\necho 0.0.0\n' > "$work/scripts/release_version.sh"
cat > "$work/scripts/cpkt-toolchains.sh" <<'SH'
#!/bin/sh
case "$1" in
  env) [ "$ENV_FAIL" = 0 ] || exit 7; echo 'export CC=fixture LD=fixture AR=fixture RANLIB=fixture' ;;
esac
SH
for tool in cmake luarocks lua; do printf '#!/bin/sh\nexit 0\n' > "$work/commands/$tool"; done
cat > "$work/commands/cmake" <<'SH'
#!/bin/sh
if [ "$1" = --install ]; then mkdir -p "$4/lib/pkgconfig"; fi
SH
cat > "$work/commands/ctest" <<'SH'
#!/bin/sh
printf ctest >> "$LUA_CALLS"
exit "$RUNTIME_FAIL"
SH
chmod +x "$work/scripts/"* "$work/commands/"*
export PATH="$work/commands:$PATH" LUA_CALLS="$work/calls"
for runner in lua-test.sh lua-debug.sh; do
  for state in missing success runtime_failure resolver_failure; do
    export SELECT_BOOTLIN=1 RUNTIME_FAIL=0 ENV_FAIL=0
    expected=0
    case "$state" in
      missing) SELECT_BOOTLIN=0; expected=1 ;;
      runtime_failure) RUNTIME_FAIL=9; expected=9 ;;
      resolver_failure) ENV_FAIL=1; expected=7 ;;
    esac
    : > "$LUA_CALLS"
    status=0
    "$work/scripts/$runner" > "$work/log" 2>&1 || status=$?
    [[ "$status" == "$expected" ]] || { cat "$work/log"; exit 1; }
    if [[ "$state" == success || "$state" == runtime_failure ]]; then [[ -s "$LUA_CALLS" ]]; else [[ ! -s "$LUA_CALLS" ]]; fi
  done
done
echo 'Lua workflows propagate runtime/CTest failure and refuse unavailable toolchains.'
