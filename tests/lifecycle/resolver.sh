#!/usr/bin/env bash
set -euo pipefail
fixture=$1
build=$2
work=$(mktemp -d "$build/resolver.XXXXXX")
server=
cleanup() {
  if [[ -n "$server" ]]; then kill "$server" 2>/dev/null || true; wait "$server" 2>/dev/null || true; fi
  rm -rf "$work"
}
trap cleanup EXIT
root=$(cd "${BASH_SOURCE[0]%/*}/../.." && pwd)
python3 "$root/tests/fixtures/dns_server.py" "$work" &
server=$!
deadline=$((SECONDS + 5))
until [[ -f "$work/port6" ]]; do
  kill -0 "$server"
  ((SECONDS < deadline)) || { echo 'DNS data fixture did not start' >&2; exit 1; }
  sleep 0.01
done
port=$(cat "$work/port")
servers="127.0.0.1:$port"
printf '192.0.2.9 hosts.test alias-host # comment\n2001:db8::9 hosts6.test\n192.0.2.10 dns.test\n' > "$work/hosts"
printf 'nameserver 127.0.0.1\nsearch suffix.test\noptions ndots:1 attempts:1\n' > "$work/resolv"
check() {
  local name=$1 expected=$2 timeout=${3:-1200} actual
  actual=$("$fixture" "$name" "$work/hosts" "$work/resolv" "$servers" "$timeout")
  diff -u <(printf '%s\n' "$expected" | sort) <(printf '%s\n' "$actual" | sort)
}
reject() {
  if "$fixture" "$1" "$work/hosts" "$work/resolv" "$servers" "${2:-1200}" > "$work/out"; then
    echo "ERROR: resolver accepted invalid/unavailable query $1" >&2; exit 1
  fi
  [[ ! -s "$work/out" ]]
}
check 127.0.0.1 '127.0.0.1 6010'
check ::1 '::1 6010'
index=$(cat /sys/class/net/lo/ifindex)
check fe80::1%lo "fe80::1%$index 6010"
check "fe80::1%$index" "fe80::1%$index 6010"
check fe80::1%0 'fe80::1 6010'
for name in fe80::1% fe80::1%4294967296 fe80::1%no-such-iface; do reject "$name"; done
check hosts.test '192.0.2.9 6010'
check alias-host '192.0.2.9 6010'
check hosts6.test '2001:db8::9 6010'
check dns.test '192.0.2.10 6010'
[[ ! -e "$work/queries" ]]
: > "$work/hosts"
for name in dns.test alias.test short tcp.test wrong-port.test cross.test; do check "$name" $'192.0.2.51 6010\n2001:db8::51 6010'; done
grep -q 'tcp.test .* 1$' "$work/queries"
grep -q 'short.suffix.test ' "$work/queries"
reject absent.test
reject broken.test 200
reject silent.test 120
reject dns.test 0
servers='not a server'; reject dns.test
servers="[::1]:$(cat "$work/port6")"
for name in dns.test wrong-port.test cross.test; do check "$name" $'192.0.2.51 6010\n2001:db8::51 6010'; done
echo 'Resolver precedence, IPv6, CNAME/search, TCP fallback, reply origin and failure cases passed.'
