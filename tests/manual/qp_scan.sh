#!/usr/bin/env bash
# Sweeps queue pair counts and checks the accounting at each one.
#
# What it is looking for is not throughput but balance: every sub-operation
# posted has to complete exactly once. An unbalanced count means work requests
# were posted with no way to reclaim them, which is invisible in a
# single-queue-pair run and only appears once the queues rotate.
#
#   ./qp_scan.sh <path-to-rdma_loopback> [ip] [--gpu N]
set -u

BIN="${1:?usage: qp_scan.sh <rdma_loopback binary> [ip] [gpu args...]}"
IP="${2:-127.0.0.1}"
shift 2 || shift 1 || true
EXTRA="$*"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

fail=0
for n in 1 2 4 8 16; do
  "$BIN" server --qp "$n" $EXTRA > "$TMP/srv.log" 2>&1 &
  srv_pid=$!
  sleep 2
  out="$("$BIN" client "$IP" --qp "$n" $EXTRA 2>&1)"
  kill "$srv_pid" 2>/dev/null
  wait "$srv_pid" 2>/dev/null

  bal="$(printf '%s' "$out" | grep 'sub-operations:' | head -1 | sed 's/^ *//')"
  read_ok="$(printf '%s' "$out" | grep -c 'data verified: OK')"
  write_ok="$(grep -c 'verifying what the client wrote: OK' "$TMP/srv.log")"
  handoff="$(grep -oE 'handoffs received: [0-9]+' "$TMP/srv.log" | head -1)"

  printf 'qp=%-3s %s | read=%s write=%s | %s\n' \
         "$n" "$bal" "$read_ok" "$write_ok" "$handoff"

  case "$bal" in *UNBALANCED*) fail=1 ;; esac
  [ "$read_ok" = "1" ] || fail=1
  [ "$write_ok" = "1" ] || fail=1
  sleep 1
done

if [ "$fail" = "0" ]; then
  echo "all queue pair counts balanced"
else
  echo "FAILED"
fi
exit "$fail"
