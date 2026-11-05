#!/usr/bin/env bash
# tools/build-slot.sh, the machine-wide heavy lock: capacity one, the lowest
# priority number goes first (FIFO within one), a dead owner is reaped, a live
# one is respected until the wait cap, a nested call does not deadlock.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SLOT="$ROOT/tools/build-slot.sh"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
export SONORA_HEAVY_DIR="$T/lock" SONORA_HEAVY_POLL=0.2
unset SONORA_HEAVY_HELD SONORA_HEAVY_PRIORITY
fail=0
bad() { echo "FAIL: $*" >&2; fail=1; }

job() {  # name [priority]: log start and end, holding the lock in between
  # shellcheck disable=SC2016  # expanded by the inner shell
  SONORA_HEAVY_PRIORITY="${2:-2}" "$SLOT" bash -c 'echo "start $1" >>"$2"; sleep "$3"; echo "end $1" >>"$2"' _ "$1" "$T/log" "${3:-1}"
}

# Capacity one: the second run waits for the first.
job a 2 3 2>"$T/err-a" & pa=$!
sleep 0.5
job b 2>"$T/err-b" & pb=$!
wait "$pa" "$pb"
[[ "$(tr '\n' ' ' <"$T/log")" == "start a end a start b end b " ]] || bad "runs overlapped: $(tr '\n' ' ' <"$T/log")"
grep -q 'waiting for the heavy lock' "$T/err-b" || bad "second run did not queue"
[[ ! -d "$T/lock/slot-0" ]] || bad "lock not released"

# Priority: while a holds it, c (2) arrives before d (1) and e (0); the order
# is a, e, d, c; two of equal priority keep their arrival order.
: >"$T/log"
job a 2 4 2>/dev/null & pa=$!
sleep 0.5
job c 2 2>/dev/null & pc=$!
sleep 0.3
job d 1 2>/dev/null & pd=$!
sleep 0.3
job e 0 2>/dev/null & pe=$!
sleep 0.3
job f 0 2>/dev/null & pf=$!
wait "$pa" "$pc" "$pd" "$pe" "$pf"
order="$(grep '^start' "$T/log" | cut -d' ' -f2 | tr -d '\n')"
[[ "$order" == aefdc ]] || bad "priority order $order, want aefdc"

# The holder's owner, command and start time are recorded.
# shellcheck disable=SC2016  # expanded by the inner shell
"$SLOT" bash -c 'cat "$1"/slot-0/owner' _ "$T/lock" >"$T/owner"
for k in pid= start= cmd=; do grep -q "^$k" "$T/owner" || bad "owner file lacks $k"; done

# A dead owner's lock is taken over.
mkdir -p "$T/lock/slot-0"
printf 'pid=%s\nkind=%s\n' "$(bash -c 'echo $$')" "$(case "$(uname -s)" in MINGW* | MSYS* | CYGWIN*) echo win ;; *) echo linux ;; esac)" >"$T/lock/slot-0/owner"
out="$("$SLOT" echo ran 2>&1)"
[[ "$out" == ran ]] || bad "stale lock not reaped: $out"

# A live owner is respected; the wait cap then runs the command anyway.
mkdir -p "$T/lock/slot-0"
printf 'pid=%s\nkind=%s\n' "$$" "$(case "$(uname -s)" in MINGW* | MSYS* | CYGWIN*) echo win ;; *) echo linux ;; esac)" >"$T/lock/slot-0/owner"
out="$(SONORA_HEAVY_WAIT=1 "$SLOT" echo ran 2>&1)"
{ grep -q 'running anyway' <<<"$out" && grep -q '^ran$' <<<"$out"; } || bad "wait cap: $out"
rm -rf "$T/lock/slot-0"

# A script that wraps itself and calls another wrapped script does not wait
# for itself.
out="$(SONORA_HEAVY_WAIT=5 "$SLOT" "$SLOT" echo nested 2>&1)"
[[ "$out" == nested ]] || bad "nested call: $out"

# The capacity is the machine's: heavy.conf, not the caller's environment.
echo 'SONORA_HEAVY_SLOTS=2' >"$T/lock/heavy.conf"
j2="$(SONORA_HEAVY_SLOTS=64 "$SLOT" --jobs)"
echo 'SONORA_HEAVY_SLOTS=1' >"$T/lock/heavy.conf"
j1="$("$SLOT" --jobs)"
[[ "$j1" -ge 2 && "$j1" -ge "$j2" ]] || bad "--jobs gave $j1 with one slot, $j2 with two"
rm -f "$T/lock/heavy.conf"

# The command's exit status passes through.
rc=0
"$SLOT" bash -c 'exit 7' || rc=$?
[[ "$rc" -eq 7 ]] || bad "exit status $rc, want 7"

[[ "$fail" -eq 0 ]] || exit 1
echo "ok: heavy lock has capacity 1, orders by priority, reaps dead owners"
