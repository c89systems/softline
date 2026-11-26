#!/usr/bin/env bash
set -Eeuo pipefail
tmux=$1 fixture=$2 build=$3 suite=$4
work=$(mktemp -d "$build/cursor-matrix.XXXXXX")
cd "$work"
unset TMUX TMUX_PANE
run() { "$tmux" -S terminal.sock -f /dev/null "$@"; }
cleanup() {
  local result=$? run_capture=$work/actual
  run kill-server >/dev/null 2>&1 || :
  if ((result)); then
    [[ ! -f $run_capture ]] || cat "$run_capture" >&2
    echo "Failure captures retained in $work" >&2
  else
    rm -rf -- "$work"
  fi
}
trap cleanup EXIT
trap 'echo "line $LINENO: $BASH_COMMAND (${scenario:-setup})" >&2' ERR
wait_for() {
  local deadline=$((SECONDS + 4))
  until "$@"; do
    ((SECONDS < deadline)) || { echo "Timed out: $* ($scenario)" >&2; return 1; }
    sleep .01
  done
}
pane=matrix:0.0
mkfifo launch
exec 7<>launch
capture() { run capture-pane -p -S - -t "$pane" > actual; }
contains() { capture && grep -Fxq -- "$1" actual; }
dead() { [[ $(run display-message -p -t "$pane" '#{pane_dead}') == 1 ]]; }
start() {
  run start-server \; set-option -g history-limit "$1" \; \
    new-session -d -s matrix -x 40 -y 8 cat
  run set-option -g status off
  run set-option -g remain-on-exit on
  run set-option -g remain-on-exit-format ''
}
new_fixture() {
  local launch recording
  printf -v launch 'exec %q %q' "$BASH" "$work/runner"
  printf -v recording 'exec cat > %q' "$work/output"
  run resize-window -t matrix:0 -x 40 -y 8 \; clear-history -t "$pane" \; \
    respawn-pane -k -t "$pane" "$launch" \; pipe-pane -t "$pane" "$recording"
  printf '\n' >&7
}
runner_header() {
  printf '%s\n' '#!/usr/bin/env bash' 'set -euo pipefail'
  printf 'IFS= read -r < %q\n' "$work/launch"
}
check_exit() {
  wait_for dead
  [[ $(run display-message -p -t "$pane" '#{pane_dead_status}') == 0 ]]
  [[ -s output ]]
  if LC_ALL=C grep -Eq $'\033\\[\\??6n' output; then
    echo "Cursor-position request in $scenario" >&2
    exit 1
  fi
}
prepare_prompt() {
  {
    runner_header
    printf 'printf %q\n' "\\033[2J\\033[HOLD_ONE\\r\\nOLD_TWO\\r\\nOLD_THREE\\033[$1;1H"
    printf 'exec %q --pinned-readline %q %q\n' "$fixture" "$2" "$3"
  } > runner
  new_fixture
  wait_for contains '>'
}
check_return() {
  run send-keys -t "$pane" "$1"
  check_exit
  capture
  grep -Fxq OLD_ONE actual
  grep -Fxq OLD_TWO actual
  grep -Fxq OLD_THREE actual
  # Inspect visible rows separately so scrollback cannot hide a wrong cursor.
  run capture-pane -p -t "$pane" > actual
  [[ $(tail -2 actual | head -1) == AFTER ]]
}
case $suite in
  submit)
    start 2000
    for row in 1 4 8; do
      for draft in '' 'short' 'a deliberately long draft that wraps at the terminal edge' $'first\nlast'; do
        scenario="submit row=$row draft=$draft"
        prepare_prompt "$row" 0 "$draft"
        if [[ $draft == *$'\n'* ]]; then
          run send-keys -t "$pane" -l first
          run send-keys -t "$pane" C-j
          run send-keys -t "$pane" -l last
          wait_for contains '  last'
          run send-keys -t "$pane" Up Home
        elif [[ -n $draft ]]; then
          run send-keys -t "$pane" -l "$draft"
          if ((${#draft} > 38)); then
            wait_for contains '  at the terminal edge'
          else
            wait_for contains "> $draft"
          fi
          run send-keys -t "$pane" Home
        fi
        check_return Enter
      done
    done
    for row in 1 8; do
      scenario="pinned EOF row=$row"
      prepare_prompt "$row" 12 --eof
      check_return C-d
      scenario="pinned interrupt row=$row"
      prepare_prompt "$row" 12 --interrupt
      run send-keys -t "$pane" -l 'unfinished'
      wait_for contains '> unfinished'
      check_return C-c
    done
    ;;
  wrap)
    start 2000
    draft=abcdefghijklmnopqrstuvwxy
    wrap_rows() {
      local at=0 cells=$(( $1 - 2 )) prefix='> ' row seen=0 line
      local -a visible
      run capture-pane -p -t "$pane" > actual
      mapfile -t visible < actual
      row=$((${#visible[@]} - (${#draft} + cells - 1) / cells))
      for line in "${visible[@]}"; do
        if [[ $line == '> '* ]]; then ((++seen)); fi
      done
      [[ $seen == 1 ]] || return 1
      while ((at < ${#draft})); do
        [[ ${visible[row]} == "$prefix${draft:at:cells}" ]] || return 1
        prefix='  '
        ((at+=cells))
        ((row+=1))
      done
    }
    repainted() {
      # The same text can fit at both widths. Wait for the new paint before
      # resizing again, even when the snapshot already matches.
      LC_ALL=C tail -c "+$1" output | grep -aF -- "$2" >/dev/null
    }
    at_prompt_start() {
      [[ $(run display-message -p -t "$pane" '#{cursor_x}') == 2 ]]
    }
    for hint in 12 16 0 64; do
      scenario="wrap hint=$hint geometry=40x8"
      prepare_prompt 2 "$hint" "$draft"
      run send-keys -t "$pane" -l "$draft"
      for geometry in 40x8 30x8 54x8 10x8 40x12 24x6; do
        scenario="wrap hint=$hint geometry=$geometry"
        cols=${geometry%x*} rows=${geometry#*x}
        effective=$hint
        if ((effective == 0 || effective > cols)); then effective=$cols; fi
        if [[ $geometry != 40x8 ]]; then
          if [[ $hint == 16 && $geometry == 10x8 ]]; then
            run send-keys -t "$pane" C-a
            wait_for at_prompt_start
          fi
          from=$(wc -c < output)
          run resize-window -t matrix:0 -x "$cols" -y "$rows"
          cells=$((effective - 2))
          last=$((${#draft} - 1 - (${#draft} - 1) % cells))
          wait_for repainted "$((from + 1))" "${draft:last}"$'\033[0m\033[0K'
        fi
        wait_for wrap_rows "$effective"
      done
      check_return Enter
    done
    ;;
  append)
    mkfifo commands replies
    exec 8<>commands 9<>replies
    ack() { IFS=' ' read -r -t 4 columns_seen rows_seen <&9; }
    step() { printf %s "$1" >&8; ack; }
    resize() {
      local rows=$1 tty
      tty=$(run resize-window -t matrix:0 -x 40 -y "$rows" \; \
        display-message -p -t "$pane" '#{pane_tty}')
      # The terminal grid has settled. Publish the same geometry to the
      # fixture's PTY synchronously so tmux's notification timer cannot
      # affect the cost or ordering of these cases.
      stty -F "$tty" rows "$rows" cols 40
      step r
      [[ $columns_seen == 40 && $rows_seen == "$rows" ]]
    }
    ascii=0123456789012345678901234567890123456789
    unicode=${ascii%?}é
    policies=(cleared disabled limited retained)
    if [[ ${5-} ]]; then policies=("$5"); fi
    for policy in "${policies[@]}"; do
      limit=2000
      if [[ $policy == disabled ]]; then limit=0; fi
      if [[ $policy == limited ]]; then limit=2; fi
      start "$limit"
      prefix=$'ROW_ONE\nROW_TWO\nROW_THREE\nROW_FOUR\nROW_FIVE\n'
      if [[ $policy == cleared || $policy == disabled ]]; then prefix=''; fi
      for kind in short ascii unicode; do
        case $kind in
          short) source=ABC ;;
          ascii) source=$ascii ;;
          unicode) source=$unicode ;;
        esac
        case $policy/$kind in
          cleared/short) tails=(p repeat n h split-sgr split-utf8 invalid) ;;
          cleared/ascii) tails=(p style repeat n h) ;;
          cleared/unicode) tails=(repeat h) ;;
          disabled/short) tails=(p repeat) ;;
          disabled/ascii) tails=(repeat n h) ;;
          limited/short|retained/ascii) tails=(p) ;;
          limited/ascii) tails=(style) ;;
          *) tails=(repeat) ;;
        esac
        for tail in "${tails[@]}"; do
          scenario="append history=$policy source=$source tail=$tail"
          {
            runner_header
            printf '%s\n' "printf '\\033[2J\\033[H'"
            printf 'exec %q --gated-shell %q %q %q\n' "$fixture" "$work/commands" "$work/replies" \
              "$prefix$source"
          } > runner
          new_fixture
          ack
          step s
          wait_for contains "$source"
          if [[ $policy == cleared ]]; then run clear-history -t "$pane"; fi
          capture
          cp actual before
          case $tail in
            split-sgr) step a ;;
            split-utf8) step u ;;
          esac
          resize 10
          step c
          if [[ $tail == style || $tail == repeat ]]; then step S; fi
          if [[ $tail == repeat ]]; then resize 13; step c; step S; fi
          case $tail in
            n) step n ;;
            h) step h ;;
            split-sgr) step A; step p ;;
            split-utf8) step U; step p ;;
            invalid) step v; step p ;;
            *) step p ;;
          esac
          if [[ $tail == h ]]; then
            expected=Y${source:1}
          elif [[ $tail == n || $source != ABC ]]; then
            expected=Y
          else
            expected=${source}Y
          fi
          case $tail in
            split-sgr) expected=${source}COLORY ;;
            split-utf8) expected=${source}─Y ;;
          esac
          wait_for contains "$expected"
          if [[ $expected == Y ]]; then
            awk -v source="$source" '$0 == "Y" && prior == source { found=1 } { prior=$0 } END { exit !found }' actual
          fi
          awk 'FNR == NR { if ($0 ~ /^ROW_/) before[$0]++; next }
               /^ROW_/ { after[$0]++ }
               END { for (row in before) if (after[row] != before[row]) exit 1 }' before actual
          printf x >&8
          check_exit
        done
      done
      run kill-server
    done
    ;;
  *) exit 2 ;;
esac
