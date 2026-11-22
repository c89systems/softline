#!/usr/bin/env bash
set -euo pipefail
root=$1
shift
work=$(mktemp -d "$root/build/queue-display-option.XXXXXX")
trap 'rm -rf -- "$work"' EXIT
for delay in '' -1 2147483648 bad 1.5 '+1' ' 1'; do
  if printf '/quit\n' | SOFTLINE_QUEUE_DISPLAY_DELAY_MS="$delay" "$@" >"$work/log" 2>&1; then
    echo "Invalid queue display delay accepted: '$delay'" >&2
    exit 1
  fi
  grep -q SOFTLINE_QUEUE_DISPLAY_DELAY_MS "$work/log" || { cat "$work/log"; exit 1; }
done
for delay in 0 120 500 2147483647; do
  printf '/quit\n' | SOFTLINE_QUEUE_DISPLAY_DELAY_MS="$delay" "$@" >"$work/log" 2>&1 || {
    cat "$work/log"; exit 1;
  }
done
echo 'Queue visibility option validation passed.'
