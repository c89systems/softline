#!/usr/bin/env bash
set -euo pipefail
tmux=$1
fixture=$2
build=$3
work=$(mktemp -d "$build/native-tmux.XXXXXX")
# Relative Unix socket paths stay below the kernel limit in extracted sources.
cd "$work"
socket=tmux.sock
cleanup() {
  "$tmux" -S "$socket" kill-server >/dev/null 2>&1 || :
  rm -rf -- "$work"
}
trap cleanup EXIT
run() { "$tmux" -S "$socket" -f /dev/null "$@"; }
mkfifo commands replies
exec 8<>commands 9<>replies
live_ack() {
  timeout 4 dd bs=4 count=1 iflag=fullblock status=none <&9 > acknowledgment
  [[ $(wc -c < acknowledgment) == 4 ]]
}
live_step() { printf %s "$1" >&8; live_ack; }
live_resize() {
  local rows=$1 columns=${2:-16} observed_columns observed_rows deadline=$((SECONDS + 4))
  run resize-window -t unicode:0 -x "$columns" -y "$rows"
  while :; do
    live_step r
    read -r observed_columns observed_rows < <(od -An -tu2 acknowledgment)
    [[ $observed_columns != "$columns" || $observed_rows != "$rows" ]] || return 0
    ((SECONDS < deadline)) || return 1
    sleep .01
  done
}
capture_runner() {
  local destination=$1 command deadline captured
  printf -v command 'exec %q %q' "$BASH" "$work/runner"
  run clear-history -t unicode:0.0
  run respawn-pane -k -t unicode:0.0 "$command"
  deadline=$((SECONDS + 5))
  while [[ $(run display-message -p -t unicode:0.0 '#{pane_dead}') != 1 ]]; do
    ((SECONDS < deadline)) || { echo 'Native output fixture failed to finish' >&2; exit 1; }
    sleep .01
  done
  [[ $(run display-message -p -t unicode:0.0 '#{pane_dead_status}') == 0 ]] || {
    echo 'Native output fixture failed' >&2; exit 1;
  }
  # Trim terminal padding while retaining row breaks and the final cursor.
  captured=$(run capture-pane -p -S - -t unicode:0.0)
  printf '%s\n' "$captured" > "$destination"
  run display-message -p -t unicode:0.0 '#{cursor_y},#{cursor_x}' >> "$destination"
}
render() {
  local text=$1 chunk=$2 destination=$3
  {
    printf '%s\n' '#!/usr/bin/env bash' 'set -euo pipefail' "printf '\\033[2J\\033[H'"
    if ((chunk)); then
      printf 'exec %q --chunks %q %q\n' "$fixture" "$text" "$chunk"
    else
      printf 'printf '\''\\033[r\\033[65535;1H\\033D%%s\\r\\n'\'' %q\n' "$text"
    fi
  } > "$work/runner"
  capture_runner "$destination"
}
run new-session -d -s unicode -x 40 -y 8 cat
run set-option -g remain-on-exit on
run set-option -g remain-on-exit-format ''
run set-option -g status off
repeat() {
  local glyph=$1 count=$2 result= index
  for ((index=0; index<count; ++index)); do result+=$glyph; done
  printf '%s' "$result"
}
texts=("$(repeat a 40)X" "$(repeat '🇸🇪' 22)X" "$(repeat '👩‍💻' 10)X"
  "$(repeat '❤️' 22)X" "$(repeat '1️⃣' 22)X" "$(repeat a 39)éX" "$(repeat a 40)́X")
for text in "${texts[@]}"; do
  render "$text" 0 "$work/expected"
  # Baseline must display the sentinel before the separate cursor record.
  [[ $(tail -2 "$work/expected" | head -1) == *X ]] || { cat "$work/expected"; exit 1; }
  length=$(printf %s "$text" | wc -c)
  mapfile -t chunks < <(printf '%s\n' 1 4 8 "$length" | sort -nu)
  for chunk in "${chunks[@]}"; do
    render "$text" "$chunk" "$work/actual"
    cmp "$work/expected" "$work/actual" || {
      diff -u "$work/expected" "$work/actual" >&2 || :
      echo "tmux mismatch: chunk=$chunk source=$text" >&2
      exit 1
    }
  done
done

# Ctrl-L repairs owned cells and margins even at unchanged dimensions, without
# disturbing an unfinished producer line or its incremental fragment parser.
for kind in plain ansi utf8 panels edge; do
  draft='keep café draft'
  expected_cursor=11,15
  if [[ $kind == edge ]]; then draft="$(repeat a 37)Z"; expected_cursor=11,38; fi
  run resize-window -t unicode:0 -x 40 -y 12
  run clear-history -t unicode:0.0
  {
    printf '%s\n' '#!/usr/bin/env bash' 'set -euo pipefail' "printf '\\033[2J\\033[H'"
    printf 'exec %q --gated-live %q %q prefix\n' "$fixture" "$work/commands" "$work/replies"
  } > runner
  printf -v launch 'exec %q %q' "$BASH" "$work/runner"
  run respawn-pane -k -t unicode:0.0 "$launch"
  live_ack
  live_step s
  printf i >&8
  live_ack
  run send-keys -t unicode:0.0 -l "$draft"
  run send-keys -t unicode:0.0 Left Left
  deadline=$((SECONDS + 4))
  while :; do
    run capture-pane -p -S - -t unicode:0.0 > expected-reset
    [[ $(run display-message -p -t unicode:0.0 '#{cursor_y},#{cursor_x}') == "$expected_cursor" ]] &&
      grep -Fq "$draft" expected-reset && break
    ((SECONDS < deadline)) || exit 1
    sleep .01
  done
  live_step r
  margin=10
  if [[ $kind == panels ]]; then live_step t; live_step r; margin=7; fi
  before=$(run display-message -p -t unicode:0.0 '#{cursor_y},#{cursor_x}')
  run capture-pane -p -S - -t unicode:0.0 > expected-reset
  case $kind in ansi) live_step a ;; utf8) live_step u ;; esac
  live_step d
  run capture-pane -p -t unicode:0.0 > damaged
  grep -Fq BROKEN damaged
  run send-keys -t unicode:0.0 C-l
  deadline=$((SECONDS + 4))
  while :; do
    run capture-pane -p -S - -t unicode:0.0 > actual-reset
    cmp -s expected-reset actual-reset && break
    ((SECONDS < deadline)) || { echo 'Ctrl-L did not repair the owned prompt' >&2; cat actual-reset >&2; exit 1; }
    sleep .01
  done
  [[ $(run display-message -p -t unicode:0.0 '#{cursor_y},#{cursor_x}') == "$before" ]]
  [[ $(run display-message -p -t unicode:0.0 '#{scroll_region_lower}') == "$margin" ]]
  case $kind in ansi) live_step A; output=prefixCOLORY ;; utf8) live_step U; output=prefix─Y ;; *) output=prefixY ;; esac
  live_step p
  run capture-pane -p -J -S - -t unicode:0.0 > actual-reset
  [[ $(grep -Fxc "$output" actual-reset) == 1 ]]
  if [[ $kind != edge ]]; then
    live_resize 8 24
    run send-keys -t unicode:0.0 C-l
    live_step r
    [[ $(run display-message -p -t unicode:0.0 '#{scroll_region_lower}') == "$((margin - 4))" ]]
    [[ $(run display-message -p -t unicode:0.0 '#{cursor_y},#{cursor_x}') == "7,${before#*,}" ]]
    run capture-pane -p -J -S - -t unicode:0.0 > actual-reset
    [[ $(grep -Fxc "$output" actual-reset) == 1 ]]
    grep -Fq "$draft" actual-reset
  fi
  printf x >&8
  deadline=$((SECONDS + 4))
  while [[ $(run display-message -p -t unicode:0.0 '#{pane_dead}') != 1 ]]; do
    ((SECONDS < deadline)) || exit 1
    sleep .01
  done
  [[ $(run display-message -p -t unicode:0.0 '#{pane_dead_status}') == 0 ]]
done

run resize-window -t unicode:0 -x 60 -y 24
check_quotes() {
  local label=$1 initial=$2 between=$3 outside=$4 expected=$5
  {
    printf '#!/usr/bin/env bash\nset -euo pipefail\n'
    printf 'exec %q --quote-spacing %q %q %q\n' "$fixture" "$initial" "$between" "$outside"
  } > "$work/runner"
  capture_runner "$work/actual"
  # The first stream starts at the bottom of the existing viewport. The
  # preexisting text survives, and every separator after FIRST stays exact.
  printf '%s\n' "$expected" | sed -n '/^> FIRST$/,$p' > "$work/expected"
  sed '$d' "$work/actual" | sed -n '/^> FIRST$/,$p' > "$work/quotes"
  if [[ -n $initial ]]; then
    diff -u <(printf '%s\n' "$initial" | sed '/^$/d') \
      <(sed '/^> FIRST$/,$d; /^$/d' "$work/actual")
  fi
  [[ $(tail -1 "$work/actual") == 23,0 ]]
  cmp "$work/expected" "$work/quotes" || {
    echo "Quote spacing mismatch: $label" >&2
    diff -u "$work/expected" "$work/quotes" >&2 || :
    exit 1
  }
  echo "Quote spacing passed: $label"
}
check_quotes shell $'SHELL$\n' '' '' $'SHELL$\n\n> FIRST\n\n> SECOND\n\n> THIRD'
check_quotes banner $'SHELL$\nHOST BANNER\n' '' '' $'SHELL$\nHOST BANNER\n\n> FIRST\n\n> SECOND\n\n> THIRD'
check_quotes unfinished 'UNFINISHED' '' '' $'UNFINISHED\n\n> FIRST\n\n> SECOND\n\n> THIRD'
check_quotes empty '' '' '' $'\n> FIRST\n\n> SECOND\n\n> THIRD'
for tail in '' $'\n' $'\n\n'; do
  check_quotes "answer with ${#tail} trailing LFs" $'BANNER\n' "ANSWER$tail" '' \
    $'BANNER\n\n> FIRST\n\nANSWER\n\n> SECOND\n\n> THIRD'
done
check_quotes 'preserve excess spacing' $'BANNER\n' $'ANSWER\n\n\n' '' \
  $'BANNER\n\n> FIRST\n\nANSWER\n\n\n> SECOND\n\n> THIRD'
check_quotes 'external unfinished line on reopen' $'BANNER\n' '' 'OUTSIDE' \
  $'BANNER\n\n> FIRST\n\n> SECOND\n\nOUTSIDE\n\n> THIRD'

# Settle each input-owned resize without emitting producer bytes between them.
# Repeated clipping must not turn an unfinished line into a hard boundary.
for kind in unfinished boundary cr unicode wrap close; do
  source=prefix expected_row=0
  case $kind in
    boundary) source=$'prefix\n'; expected_row=2 ;;
    cr) source=$'prefix\r' ;;
    unicode) source=café ;;
    wrap) source=0123456789012345678901234567890123456789 ;;
  esac
  run resize-window -t unicode:0 -x 40 -y 12
  run clear-history -t unicode:0.0
  {
    printf '%s\n' '#!/usr/bin/env bash' 'set -euo pipefail' "printf '\\033[2J\\033[H'"
    printf 'exec %q --gated-live %q %q %q\n' "$fixture" "$work/commands" "$work/replies" "$source"
  } > runner
  printf -v launch 'exec %q %q' "$BASH" "$work/runner"
  run respawn-pane -k -t unicode:0.0 "$launch"
  live_ack
  live_step s
  printf i >&8
  live_ack
  # Grow and clear the draft to move the producer above the rows that will
  # survive the next shrink. A fresh native stream starts at the bottom.
  live_step b
  live_step r
  live_step e
  live_step r
  live_resize 5
  live_resize 4
  if [[ $kind != close ]]; then
    live_step p
    run capture-pane -p -t unicode:0.0 > visible
    [[ $(sed -n "$((expected_row + 1))p" visible) == Y ]] || {
      echo "Repeated clipping misplaced $kind continuation; expected row $expected_row" >&2
      cat visible >&2
      exit 1
    }
    [[ $(grep -Ec '^> *$' visible) == 1 ]]
    [[ $(run display-message -p -t unicode:0.0 '#{cursor_y},#{cursor_x}') == 3,2 ]]
  fi
  run capture-pane -p -S - -t unicode:0.0 > before-close
  sed -E 's/^> *$//' before-close > expected-close
  printf x >&8
  deadline=$((SECONDS + 4))
  while [[ $(run display-message -p -t unicode:0.0 '#{pane_dead}') != 1 ]]; do
    ((SECONDS < deadline)) || exit 1
    sleep .01
  done
  [[ $(run display-message -p -t unicode:0.0 '#{pane_dead_status}') == 0 ]]
  [[ $(run display-message -p -t unicode:0.0 '#{cursor_y},#{cursor_x}') == 3,0 ]]
  run capture-pane -p -S - -t unicode:0.0 > after-close
  cmp expected-close after-close
  run capture-pane -p -J -S - -t unicode:0.0 > history
  expected_source=${source%$'\n'}
  expected_source=${expected_source%$'\r'}
  [[ $(grep -Fxc "$expected_source" history) == 1 ]] || {
    echo "Repeated clipping lost or replayed $kind source" >&2
    cat history >&2
    exit 1
  }
done
