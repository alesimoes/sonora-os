#!/usr/bin/env bash
# tools/build-job.sh, tools/build-wait.sh and build-slot.sh --detach: a
# command started detached still waits for the heavy lock, outlives the
# command that started it, and its result comes back through a bounded wait
# that may be called again; one unfinished job per checkout; a cancelled job
# stops its container. Runs against a temporary lock and a fake docker.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SLOT="$ROOT/tools/build-slot.sh"
JOB="$ROOT/tools/build-job.sh"
WAIT="$ROOT/tools/build-wait.sh"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
export SONORA_HEAVY_DIR="$T/lock" SONORA_HEAVY_POLL=0.2 SONORA_BUILD_JOBS_DIR="$T/jobs" SONORA_BUILD_WAIT_POLL=0.2
unset SONORA_HEAVY_HELD SONORA_HEAVY_PRIORITY SONORA_HEAVY_SLOT_DIR
fail=0
bad() { echo "FAIL: $*" >&2; fail=1; }
job_id() { sed -n 's/^build-job: started \([^:]*\):.*/\1/p' <<<"$1"; }

# Started detached: the starting call returns at once; the wait times out
# with 75 while it runs, then returns the command's own status and output.
out="$("$SLOT" --detach bash -c 'echo begin; sleep 3; echo end; exit 4')"
id="$(job_id "$out")"
[[ -n "$id" ]] || bad "no job id in: $out"
rc=0
w="$("$WAIT" "$id" --max 0)" || rc=$?
[[ "$rc" -eq 75 ]] || bad "wait while running: rc $rc, $w"
[[ "$w" == *"call tools/build-wait.sh $id again"* ]] || bad "wait while running says: $w"

# A second start in the same checkout is refused while the first runs.
rc=0
o2="$("$JOB" start true 2>&1)" || rc=$?
[[ "$rc" -eq 3 && "$o2" == *"$id"* ]] || bad "second job: rc $rc, $o2"

rc=0
w="$("$WAIT" "$id" --max 30)" || rc=$?
[[ "$rc" -eq 4 ]] || bad "finished job: rc $rc, want 4: $w"
[[ "$w" == *begin* && "$w" == *end* ]] || bad "log tail missing: $w"
[[ ! -d "$T/lock/slot-0" ]] || bad "lock not released after the job"

# The job takes its turn: while the lock is held, it queues and says so.
mkdir -p "$T/lock/slot-0"
printf 'pid=%s\nkind=%s\n' "$$" "$(case "$(uname -s)" in MINGW* | MSYS* | CYGWIN*) echo win ;; *) echo linux ;; esac)" >"$T/lock/slot-0/owner"
id="$(job_id "$("$SLOT" --detach bash -c 'echo ran')")"
rc=0
w="$("$WAIT" "$id" --max 1)" || rc=$?
[[ "$rc" -eq 75 && "$w" == *"waiting for the heavy lock"* ]] || bad "queued job: rc $rc, $w"
rm -rf "$T/lock/slot-0"
rc=0
w="$("$WAIT" "$id" --max 30)" || rc=$?
[[ "$rc" -eq 0 && "$w" == *ran* ]] || bad "queued job after the lock freed: rc $rc, $w"

# Without an id, wait collects this checkout's newest job.
rc=0
w="$("$WAIT" --max 5)" || rc=$?
[[ "$rc" -eq 0 && "$w" == *"$id finished"* ]] || bad "wait without id: rc $rc, $w"

# A cancelled job stops its container and frees the lock.
mkdir -p "$T/bin"
cat >"$T/bin/docker" <<'FAKE'
#!/usr/bin/env bash
echo "$*" >>"$FAKE_DIR/docker.log"
case "$1" in
  inspect) [[ -f "$FAKE_DIR/running-${*: -1}" ]] && echo true || exit 1 ;;
  kill) rm -f "$FAKE_DIR/running-$2" ;;
  run)
    name=""; prev=""
    for a in "$@"; do [[ "$prev" == --name ]] && name="$a"; prev="$a"; done
    touch "$FAKE_DIR/running-$name"
    while [[ -f "$FAKE_DIR/running-$name" ]]; do sleep 0.1; done ;;
esac
FAKE
chmod +x "$T/bin/docker"
export FAKE_DIR="$T"
id="$(job_id "$(PATH="$T/bin:$PATH" "$SLOT" --detach docker run --rm img true)")"
for _ in $(seq 100); do ls "$T"/running-* >/dev/null 2>&1 && break; sleep 0.1; done
ls "$T"/running-* >/dev/null 2>&1 || bad "the job's container never started"
PATH="$T/bin:$PATH" "$JOB" cancel "$id" >/dev/null
grep -q '^kill sonora-heavy-' "$T/docker.log" || bad "cancel did not kill the container: $(cat "$T/docker.log")"
! ls "$T"/running-* >/dev/null 2>&1 || bad "container left running after cancel"
[[ -f "$T/jobs/$id/rc" ]] || bad "cancelled job has no result"
[[ ! -d "$T/lock/slot-0" ]] || bad "lock not released by the cancelled job"

# A nested docker run (micro-build.sh under build-slot.sh) is recorded in the
# holder's owner file, so the slot is held while it runs.
: >"$T/docker.log"
# shellcheck disable=SC2016  # expanded by the inner shell
PATH="$T/bin:$PATH" "$SLOT" bash -c '"$1" docker run --rm img true & sleep 1; cat "$2"/slot-0/owner; docker kill "$(sed -n "s/^container=//p" "$2"/slot-0/owner | tail -n 1)"; wait' _ "$SLOT" "$T/lock" >"$T/owner" 2>/dev/null
grep -q '^container=sonora-heavy-' "$T/owner" || bad "nested docker run not recorded: $(cat "$T/owner")"

# A holder whose slot was taken over (reaped by mistake, then claimed by the
# next build) leaves the new holder's slot alone when it ends.
# shellcheck disable=SC2016  # expanded by the inner shell
"$SLOT" bash -c 'printf "pid=1\nkind=x\n" >"$1"/slot-0/owner' _ "$T/lock"
[[ -d "$T/lock/slot-0" ]] || bad "a holder removed a slot it no longer owned"
rm -rf "$T/lock/slot-0"

[[ "$fail" -eq 0 ]] || exit 1
echo "ok: detached jobs queue for the lock, outlive their caller, report through a bounded wait, cancel their container"
