#!/usr/bin/env bash
set -euo pipefail
tmux=$1 fixture=$2 build=$3
work=$(mktemp -d "$build/retained-fragment.XXXXXX")
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
trap 'echo "line $LINENO: $BASH_COMMAND (kind=${kind:-setup} filled=${filled:-setup})" >&2' ERR
wait_for() {
  local deadline=$((SECONDS + 4))
  until "$@"; do
    ((SECONDS < deadline)) || { echo "Timed out: $*" >&2; return 1; }
    sleep .01
  done
}
ack() {
  timeout 4 dd bs=4 count=1 iflag=fullblock status=none <&9 > acknowledgment
  [[ $(wc -c < acknowledgment) == 4 ]]
}
step() { printf %s "$1" >&8; ack; }
resize() {
  local columns=$1 rows=$2 observed_columns observed_rows deadline=$((SECONDS + 4))
  run resize-window -t retained:0 -x "$columns" -y "$rows"
  while :; do
    step r
    read -r observed_columns observed_rows < <(od -An -tu2 acknowledgment)
    [[ $observed_columns != "$columns" || $observed_rows != "$rows" ]] || return 0
    ((SECONDS < deadline)) || return 1
    sleep .01
  done
}
snapshot() {
  # Command substitution trims only terminal padding below the retained frame.
  local text
  text=$(run capture-pane -p -J -S - -t retained:0.0)
  printf '%s\n' "$text" > "$1"
}
visible() {
  snapshot actual
  grep -Eq '^retained> *$' actual
}
unchanged() {
  snapshot actual
  cmp -s expected actual
}
producer_visible() {
  snapshot actual
  grep -Fxq producer actual
}
dead() { [[ $(run display-message -p -t retained:0.0 '#{pane_dead}') == 1 ]]; }
run new-session -d -s retained -x 80 -y 24 cat
run set-option -g status off
run set-option -g remain-on-exit on
run set-option -g remain-on-exit-format ''
mkfifo commands replies
exec 8<>commands 9<>replies
for filled in 1 0; do
  for kind in ansi utf8 invalid; do
    run resize-window -t retained:0 -x 80 -y 24
    run clear-history -t retained:0.0
    {
      printf '%s\n' '#!/usr/bin/env bash' 'set -euo pipefail' "printf '\\033[2J\\033[H'"
      if ((filled)); then
        for ((row=0; row<100; ++row)); do printf 'printf "HISTORY_%03d\\r\\n"\n' "$row"; done
      fi
      printf 'exec %q --gated %q %q %q %q\n' "$fixture" "$work/commands" "$work/replies" \
        $'TRANSCRIPT_FIRST\nTRANSCRIPT_MIDDLE\nTRANSCRIPT_LAST\n' 'retained> '
    } > runner
    printf -v launch 'exec %q %q' "$BASH" "$work/runner"
    run respawn-pane -k -t retained:0.0 "$launch"
    timeout 4 dd bs=4 count=1 iflag=fullblock status=none <&9 > acknowledgment
    [[ $(wc -c < acknowledgment) == 4 ]]
    step s
    printf i >&8
    wait_for visible
    run send-keys -t retained:0.0 Enter
    timeout 4 dd bs=4 count=1 iflag=fullblock status=none <&9 > acknowledgment
    [[ $(wc -c < acknowledgment) == 4 ]]
    snapshot expected
    printf -v recording 'exec cat > %q' "$work/output"
    run pipe-pane -t retained:0.0 "$recording"
    resize 80 30
    step c
    before=$(run display-message -p -t retained:0.0 '#{cursor_y},#{cursor_x}')
    case $kind in ansi) step a ;; utf8) step u ;; invalid) step v ;; esac
    after=$(run display-message -p -t retained:0.0 '#{cursor_y},#{cursor_x}')
    run pipe-pane -t retained:0.0
    printf -v recording 'exec cat > %q' "$work/reconciliation"
    run pipe-pane -t retained:0.0 "$recording"
    resize 80 40
    step c
    run pipe-pane -t retained:0.0
    wait_for unchanged || { echo "Transcript changed: $kind filled=$filled, cursor $before -> $after" >&2; exit 1; }
    wait_for test -s output
    ! LC_ALL=C grep -Fq $'\033[6n' output
    ! LC_ALL=C grep -Fq $'\033[6n' reconciliation
    case $kind in ansi) step A ;; utf8) step U ;; esac
    step p
    snapshot actual
    case $kind in ansi) grep -Fxq COLORY actual ;; utf8) grep -Fxq '─Y' actual ;; invalid) grep -Fxq Y actual ;; esac
    [[ $(grep -Ec '^retained> *$' actual) == 1 ]]
    for marker in TRANSCRIPT_FIRST TRANSCRIPT_MIDDLE TRANSCRIPT_LAST; do
      [[ $(grep -Fxc "$marker" actual) == 1 ]]
    done
    if ((filled)); then
      for ((row=0; row<100; ++row)); do
        printf -v marker 'HISTORY_%03d' "$row"
        [[ $(grep -Fxc "$marker" actual) == 1 ]]
      done
    fi
    [[ $before == "$after" ]] || { echo "Fragment write displaced the retained cursor: $before -> $after" >&2; exit 1; }
    step c
    step p
    snapshot actual
    case $kind in ansi) grep -Fxq COLORYY actual ;; utf8) grep -Fxq '─YY' actual ;; invalid) grep -Fxq YY actual ;; esac
    printf x >&8
    wait_for dead
    [[ $(run display-message -p -t retained:0.0 '#{pane_dead_status}') == 0 ]]
  done
done

# Growing a pane can restore fewer rows than Softline previously scrolled.
# The retained prompt and a prompt-free producer both have a physical cursor
# to anchor the next fragment, regardless of the terminal's history policy.
run kill-server
for history in cleared disabled limited retained; do
  limit=2000
  if [[ $history == disabled ]]; then limit=0; fi
  if [[ $history == limited ]]; then limit=2; fi
  run start-server \; set-option -g history-limit "$limit" \; \
    new-session -d -s retained -x 40 -y 8 cat
  run set-option -g status off
  run set-option -g remain-on-exit on
  run set-option -g remain-on-exit-format ''
  for owner in producer prompt; do
    run resize-window -t retained:0 -x 40 -y 8
    run clear-history -t retained:0.0
    {
      printf '%s\n' '#!/usr/bin/env bash' 'set -euo pipefail' "printf '\\033[2J\\033[H'"
      printf 'exec %q --gated %q %q %q %q\n' "$fixture" "$work/commands" "$work/replies" \
        $'ROW_ONE\nROW_TWO\nROW_THREE\nROW_FOUR\nROW_FIVE\nproducer' 'retained> '
    } > runner
    run respawn-pane -k -t retained:0.0 "$launch"
    ack
    step s
    if [[ $owner == prompt ]]; then
      printf i >&8
      wait_for visible
      run send-keys -t retained:0.0 Enter
      ack
    fi
    wait_for producer_visible
    if [[ $history == cleared ]]; then run clear-history -t retained:0.0; fi
    history_size=$(run display-message -p -t retained:0.0 '#{history_size}')
    case $history in
      cleared|disabled) [[ $history_size == 0 ]] ;;
      limited) [[ $history_size == 2 ]] ;;
      retained) ((history_size > 2)) ;;
    esac
    snapshot before-growth
    sed 's/^producer$/producerY/' before-growth > expected
    printf -v recording 'exec cat > %q' "$work/growth-output"
    run pipe-pane -t retained:0.0 "$recording"
    resize 40 12
    step p
    wait_for unchanged || { echo "Append changed the screen with $history history ($owner cursor)" >&2; exit 1; }
    run pipe-pane -t retained:0.0
    if LC_ALL=C grep -Eq $'\033\\[\\??6n' growth-output; then
      echo 'Resize requested a cursor-position report' >&2
      exit 1
    fi
    printf x >&8
    wait_for dead
    [[ $(run display-message -p -t retained:0.0 '#{pane_dead_status}') == 0 ]]
  done
  run kill-server
done
