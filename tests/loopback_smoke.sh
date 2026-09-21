#!/usr/bin/env bash
# A transfer end to end through hux-bench, against this host's own adapter.
#
# The unit tests cannot catch a broken hux-bench: it is a program, not a
# target ctest runs, and it dials over a real adapter. It has been broken
# silently at least once -- a change to what a descriptor must carry made
# every import fail, and the client exited with no output while every test
# passed.
#
# Skips, rather than fails, where there is no active adapter.
set -uo pipefail

BIN="${1:?usage: loopback_smoke.sh <path to hux-bench>}"
[ -x "$BIN" ] || { echo "no hux-bench at $BIN; skipping"; exit 0; }

command -v ibv_devinfo >/dev/null 2>&1 || { echo "no ibv_devinfo; skipping"; exit 0; }
ibv_devinfo 2>/dev/null | grep -q "PORT_ACTIVE" || { echo "no active adapter; skipping"; exit 0; }

# An address on the same interface as the active port, since the provider
# selects its device and GID from the address it is told to advertise.
IP=$(ip -o addr show 2>/dev/null | awk '/inet /{print $4}' | cut -d/ -f1 \
     | grep -v '^127\.' | while read -r a; do
         if ip route get "$a" 2>/dev/null | grep -q "dev"; then echo "$a"; fi
       done | head -1)
[ -n "$IP" ] || { echo "no usable address; skipping"; exit 0; }

PORT=$(( 20000 + (RANDOM % 10000) ))
LOG=$(mktemp)
OUT=$(mktemp)
cleanup() {
  [ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null
  rm -f "$LOG" "$OUT"
}
trap cleanup EXIT

"$BIN" server --local "$IP" --port "$PORT" --sizes 65536 >"$LOG" 2>&1 &
SRV=$!
sleep 3

timeout 120 "$BIN" client "$IP" --local "$IP" --port "$PORT" \
  --sizes 65536 --iters 20 --warmup 2 >"$OUT" 2>&1
rc=$?

# The adapter may refuse a loopback connection on some fabrics; that is the
# machine, not the code. A client that produced no measurement at all and
# said nothing is the failure this exists to catch.
if [ $rc -ne 0 ] && ! grep -q "65536" "$OUT"; then
  if grep -qiE "no usable|skipping|provider create failed" "$OUT" "$LOG"; then
    echo "adapter unusable here; skipping"; cat "$OUT"; exit 0
  fi
  echo "hux-bench client failed (rc=$rc) and printed no measurement:"
  cat "$OUT"; echo "--- server ---"; cat "$LOG"
  exit 1
fi

grep -q "65536" "$OUT" || {
  echo "no row for the size that was asked for:"; cat "$OUT"; exit 1; }
grep -q "extra payload copied: 0 B" "$OUT" || {
  echo "transfer was not in place:"; cat "$OUT"; exit 1; }

echo "loopback transfer ok:"
grep -E "^65536" "$OUT"
