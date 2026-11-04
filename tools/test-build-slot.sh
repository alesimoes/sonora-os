#!/usr/bin/env bash
# tools/build-slot.sh: two runs with one slot serialize, a stale slot is
# reaped, a live one is respected until the wait cap, and --jobs is >= 2.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SLOT="$ROOT/tools/build-slot.sh"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
export TMPDIR="$T" SONORA_BUILD_SLOTS=1 SONORA_BUILD_SLOT_POLL=0.2
fail=0
bad() { echo "FAIL: $*" >&2; fail=1; }

job() {  # name: log start and end, holding the slot in between
  # shellcheck disable=SC2016  # expanded by the inner shell
  "$SLOT" bash -c 'echo "start $1" >>"$2"; sleep 1; echo "end $1" >>"$2"' _ "$1" "$T/log"
}
job a 2>"$T/err-a" & pa=$!
sleep 0.3
job b 2>"$T/err-b" & pb=$!
wait "$pa" "$pb"
expected="start a
end a
start b
end b"
[[ "$(cat "$T/log")" == "$expected" ]] || bad "runs overlapped: $(tr '\n' ' ' <"$T/log")"
grep -q 'waiting for a build slot (position 1)' "$T/err-b" || bad "second run did not queue"
[[ ! -d "$T/sonora-build-slots/slot-0" ]] || bad "slot not released"

# A dead owner's slot is taken over.
mkdir -p "$T/sonora-build-slots/slot-0"
bash -c 'echo $$' >"$T/sonora-build-slots/slot-0/pid"
out="$("$SLOT" echo ran 2>&1)"
[[ "$out" == ran ]] || bad "stale slot not reaped: $out"

# A live owner's slot is respected; the wait cap then runs the command anyway.
mkdir -p "$T/sonora-build-slots/slot-0"
echo "$$" >"$T/sonora-build-slots/slot-0/pid"
out="$(SONORA_BUILD_SLOT_WAIT=1 "$SLOT" echo ran 2>&1)"
{ grep -q 'running anyway' <<<"$out" && grep -q '^ran$' <<<"$out"; } || bad "wait cap: $out"

# The command's exit status passes through.
rm -rf "$T/sonora-build-slots/slot-0"
rc=0
"$SLOT" bash -c 'exit 7' || rc=$?
[[ "$rc" -eq 7 ]] || bad "exit status $rc, want 7"

j="$(SONORA_BUILD_SLOTS=64 "$SLOT" --jobs)"
[[ "$j" -ge 2 ]] || bad "--jobs gave $j"

[[ "$fail" -eq 0 ]] || exit 1
echo "ok: build-slot serializes, reaps stale slots, caps the wait"
