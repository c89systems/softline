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
step() {
  printf %s "$1" >&8
  timeout 4 dd bs=4 count=1 iflag=fullblock status=none <&9 > acknowledgment
  [[ $(wc -c < acknowledgment) == 4 ]]
}
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
