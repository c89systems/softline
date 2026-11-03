#!/usr/bin/env bash
set -euo pipefail
lua=$1
root=$2
case "$3" in
  packaged)
    stage=$("$root/scripts/stage_lua_rock_sources.sh" "$root/build/lua-packaged-examples")
    for file in examples/completion.lua lua/softline_terminal_lua.h; do
      [[ -f "$stage/$file" ]] || { echo "ERROR: staged Lua dependency missing: $file" >&2; exit 1; }
      grep -Fxq "$file" "$stage/RELEASE_MANIFEST"
    done
    printf '/quit\n' | "$lua" "$stage/examples/simple.lua" >/dev/null
    output=$(printf 'hello\n/quit\n' | SOFTLINE_HISTORY_DIR="$root/build/test-history/lua-packaged-examples" "$lua" "$stage/examples/chat.lua")
    [[ "$output" == '[turn] hello' ]] || { echo "ERROR: staged Lua chat output mismatch: $output" >&2; exit 1; }
    ;;
  history_auto) printf 'auto\nreadline\n' | "$lua" "$root/tests/lua_history_auto.lua" ;;
  readline) printf 'hello\n' | "$lua" "$root/tests/lua_readline.lua" ;;
  simple) printf '/quit\n' | "$lua" "$root/examples/simple.lua" >/dev/null ;;
  chat)
    output=$(printf 'hello\n/quit\n' | "$lua" "$root/examples/chat.lua")
    [[ "$output" == '[turn] hello' ]] || { echo "ERROR: non-tty Lua chat output mismatch: $output" >&2; exit 1; }
    ;;
  *) echo 'ERROR: unknown Lua console fixture' >&2; exit 2 ;;
esac
