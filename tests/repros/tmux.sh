#!/usr/bin/env bash
set -euo pipefail
tmux=$1
gate=$2
native=$3
work=$(mktemp -d "$4/terminal-repro.XXXXXX")
case_name=$5
cd "$work"
run() { "$tmux" -f /dev/null -S socket "$@"; }
cleanup() {
  run kill-server >/dev/null 2>&1 || :
  rm -rf -- "$work"
}
trap cleanup EXIT
setup() {
  if ! "$@"; then
    echo "fixture failed: $*" >&2
    run capture-pane -p -S - -t repro:0.0 >&2 || :
    exit 2
  fi
}
expect() {
  if ! "$@"; then
    echo "regression (${case_name}): $*" >&2
    cat capture >&2
    exit 1
  fi
}
dead() {
  local deadline=$((SECONDS + 3))
  until [[ $(run display-message -p -t repro:0.0 '#{pane_dead}') == 1 ]]; do
    ((SECONDS < deadline)) || return 1
    sleep .01
  done
  [[ $(run display-message -p -t repro:0.0 '#{pane_dead_status}') == 0 ]]
}
launch() {
  local width=$1 height=$2 command
  shift 2
  printf -v command '%q ' "$@"
  run respawn-pane -k -t repro:0.0 "exec $command" || return
  # Match the dimensions before starting each subsequent observation.
  [[ $(run display-message -p -t repro:0.0 '#{pane_width},#{pane_height}') == "$width,$height" ]]
}
capture() { run capture-pane -p "$@" -t repro:0.0 > capture; }
wait_row() {
  local pattern=$1 deadline=$((SECONDS + 3))
  while :; do
    capture || return
    grep -Eq "$pattern" capture && return
    ((SECONDS < deadline)) || return 1
    sleep .01
  done
}

setup run new-session -d -s repro -x 16 -y 8 cat
setup run set-option -g remain-on-exit on
setup run set-option -g remain-on-exit-format ''
setup run set-option -g status off
case "$case_name" in
  grow-cleared-history)
    mkfifo commands replies
    exec 8<>commands 9<>replies
    step() {
      printf %s "$1" >&8
      ack
    }
    ack() { timeout 3 dd bs=4 count=1 iflag=fullblock status=none <&9 > acknowledgment &&
            [[ $(wc -c < acknowledgment) == 4 ]]; }
    setup run resize-window -t repro:0 -x 40 -y 8
    printf -v source 'line-%s\n' {1..18}
    setup launch 40 8 "$gate" --gated-live "$work/commands" "$work/replies" "${source}old-output"
    setup ack
    setup step s
    setup step i
    setup wait_row '^> *$'
    setup grep -Fxq old-output capture
    setup test "$(run display-message -p -t repro:0.0 '#{history_size}')" -ge 4
    setup run clear-history -t repro:0.0
    setup test "$(run display-message -p -t repro:0.0 '#{history_size}')" = 0
    setup run resize-window -t repro:0 -x 40 -y 12
    resized() {
      local deadline=$((SECONDS+3))
      while :; do
        step r || return
        [[ $(od -An -tu2 acknowledgment | xargs) == '40 12' ]] && return
        ((SECONDS < deadline)) || return 1
      done
    }
    setup resized
    setup run send-keys -t repro:0.0 -l typed
    setup wait_row 'typed'
    capture
    expect grep -Fxq '> typed' capture
    expect grep -Fxq old-output capture
    expect test "$(grep -c '^>' capture)" = 1
    expect test "$(run display-message -p -t repro:0.0 '#{cursor_y},#{cursor_x}')" = 11,7
    ;;
  relay-after-close)
    setup launch 16 8 "$native" relay-after-close
    setup dead
    capture -S -
    expect grep -Fxq first capture
    expect grep -Fxq child capture
    expect grep -Fxq second capture
    ;;
  zero-width-wrap)
    mkfifo commands replies
    exec 8<>commands 9<>replies
    setup launch 16 8 "$native" zero-width-wrap "$work/commands" "$work/replies"
    setup read -r -t 3 initial <&9
    setup test "$initial" = 8
    setup run resize-window -t repro:0 -x 16 -y 12
    resized() {
      local rows deadline=$((SECONDS+3))
      while :; do
        printf r >&8
        read -r -t 3 rows <&9 || return
        [[ $rows == 12 ]] && return
        ((SECONDS < deadline)) || return 1
      done
    }
    setup resized
    printf x >&8
    setup dead
    capture -S -
    actual=$(sed -n '/^123456789012345e/,+1p' capture)
    expect test "$actual" = $'123456789012345e\314\201\nZ'
    ;;
  quote-without-onlcr)
    setup run resize-window -t repro:0 -x 40 -y 12
    setup launch 40 12 "$native" quote-without-onlcr
    setup dead
    capture -S -
    expect grep -Fxq answer capture
    expect grep -Fxq '> one' capture
    expect grep -Fxq '> two' capture
    ;;
  *)
    exit 2
    ;;
esac
