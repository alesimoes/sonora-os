#!/usr/bin/env bash
# Power-pull run for system.hardening.crash_recovery (ADR-0102 section 2).
#
#   crash-recovery-hw.sh arm    --session DIR --witness DIR [--tracks N] [--engine BIN]
#   crash-recovery-hw.sh verify --session DIR --witness DIR [--loss-seconds S] [--engine BIN]
#   crash-recovery-hw.sh --self-test [--engine BIN]
#
# arm records on every track with the real backend and logs 'epoch take bytes'
# once a second to a witness folder that must live on ANOTHER device (a stick
# the engine does not write to), fsynced each time, so it survives the pull.
# After the power returns, verify reopens the session and checks each take is a
# finished WAV holding at least the witnessed bytes minus the loss window.
# The default window is session-format.md's: sync_seconds (2 s) per take.
# --self-test stands in for the pull with SIGKILL and --backend fake.
set -u

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ENGINE=${SONORA_ENGINE:-$HERE/../build/sonora-engine}
LOSS_SECONDS=2
TRACKS=4
BACKEND_ARGS=()
ALLOW_SAME_DEVICE=0
SESSION=""
WITNESS=""
LOG=crash-witness.log
ENGINE_PID=""

die() { echo "crash-recovery-hw: $*" >&2; exit 2; }

cleanup() {
    if [ -n "$ENGINE_PID" ] && kill -0 "$ENGINE_PID" 2>/dev/null; then
        kill -KILL "$ENGINE_PID" 2>/dev/null
        wait "$ENGINE_PID" 2>/dev/null
    fi
}
trap cleanup EXIT

check_devices() {
    [ -n "$SESSION" ] || die "--session is required"
    [ -n "$WITNESS" ] || die "--witness is required"
    mkdir -p "$SESSION" "$WITNESS" || die "cannot create the folders"
    local a b
    a=$(stat -c %d "$SESSION") || die "cannot stat $SESSION"
    b=$(stat -c %d "$WITNESS") || die "cannot stat $WITNESS"
    if [ "$a" = "$b" ] && [ "$ALLOW_SAME_DEVICE" != 1 ]; then
        die "REFUSED: the witness is on the same device as the session (device $a); the pull would take the witness with it. Use a separate stick."
    fi
}

engine_args() {
    ENGINE_ARGS=(--dir "$SESSION" --tracks "$TRACKS" --no-memlock
                 --plugin-cache "" --autosave-seconds 0 "${BACKEND_ARGS[@]}")
}

take_sizes() { # prints 'name bytes' per take
    local f
    for f in "$SESSION"/"Audio Files"/*.wav; do
        [ -f "$f" ] || continue
        printf '%s %s\n' "$(basename "$f" | tr ' ' '_')" "$(stat -c %s "$f")"
    done
}

witness_once() { # returns 0 once any take has grown
    local now grew=1
    now=$(date +%s)
    while read -r name bytes; do
        [ -n "$name" ] || continue
        echo "$now $name $bytes" >> "$WITNESS/$LOG"
        if [ "$bytes" -gt "${FIRST[$name]:-$bytes}" ]; then grew=0; fi
        FIRST[$name]=${FIRST[$name]:-$bytes}
    done < <(take_sizes)
    sync -f "$WITNESS/$LOG" 2>/dev/null || dd if=/dev/null of="$WITNESS/$LOG" conv=notrunc,fsync status=none
    return $grew
}

cmd_arm() {
    check_devices
    [ -x "$ENGINE" ] || die "engine not found: $ENGINE"
    engine_args
    : > "$WITNESS/$LOG"
    "$ENGINE" "${ENGINE_ARGS[@]}" --arm-all --record 600 \
        < /dev/null > "$WITNESS/engine.log" 2>&1 &
    ENGINE_PID=$!
    declare -gA FIRST=()
    local said=0 limit=${ARM_SECONDS:-0} n=0
    while kill -0 "$ENGINE_PID" 2>/dev/null; do
        if witness_once && [ "$said" = 0 ]; then
            echo "PULL THE POWER NOW"
            said=1
        fi
        n=$((n + 1))
        [ "$limit" -gt 0 ] && [ "$n" -ge "$limit" ] && return 0
        sleep 1
    done
    die "the engine exited before the pull"
}

# Prints PASS/FAIL per take; exit status 1 on any FAIL.
check_takes() {
    python3 -I - "$HERE/failed-take-quit.py" "$SESSION" "$WITNESS/$LOG" "$LOSS_SECONDS" <<'PY'
import importlib.util, glob, os, struct, sys
spec = importlib.util.spec_from_file_location("ftq", sys.argv[1])
ftq = importlib.util.module_from_spec(spec); spec.loader.exec_module(ftq)
session, log, loss = sys.argv[2], sys.argv[3], float(sys.argv[4])
last = {}
for line in open(log):
    p = line.split()
    if len(p) == 3:
        last[p[1]] = int(p[2])
takes = {os.path.basename(t).replace(" ", "_"): t
         for t in glob.glob(os.path.join(session, "Audio Files", "*.wav"))}
bad = 0
if not last:
    print("FAIL: the witness log holds no takes"); sys.exit(1)
for name, want in sorted(last.items()):
    path = takes.get(name)
    if path is None:
        print(f"FAIL {name}: the take is gone"); bad += 1; continue
    _, why = ftq.check_take(path, max_short=0)
    if why:
        print(f"FAIL {name}: {why}"); bad += 1; continue
    found, blob = ftq.chunks(path)
    fmt_at, _ = found[b"fmt "]
    byte_rate = struct.unpack_from("<I", blob, fmt_at + 8)[0]
    data_at, data = found[b"data"]
    have = data_at + data
    need = want - int(loss * byte_rate)
    if have < need:
        print(f"FAIL {name}: holds {have} bytes, witnessed {want}, "
              f"allowed loss {loss} s ({int(loss * byte_rate)} bytes)"); bad += 1
    else:
        print(f"PASS {name}: holds {have} bytes, witnessed {want}")
sys.exit(1 if bad else 0)
PY
}

cmd_verify() {
    check_devices
    [ -x "$ENGINE" ] || die "engine not found: $ENGINE"
    [ -f "$WITNESS/$LOG" ] || die "no $LOG in $WITNESS"
    engine_args
    local elog="$WITNESS/verify-engine.log"
    "$ENGINE" "${ENGINE_ARGS[@]}" --headless < /dev/null > "$elog" 2>&1 &
    ENGINE_PID=$!
    for _ in $(seq 1 300); do
        grep -q "engine listening on" "$elog" 2>/dev/null && break
        kill -0 "$ENGINE_PID" 2>/dev/null || break
        sleep 0.1
    done
    if ! grep -q "engine listening on" "$elog" 2>/dev/null; then
        echo "FAIL: the reopened engine never reached 'engine listening on'"
        return 1
    fi
    kill -TERM "$ENGINE_PID" 2>/dev/null
    wait "$ENGINE_PID" 2>/dev/null
    ENGINE_PID=""
    check_takes
}

cmd_self_test() {
    [ -x "$ENGINE" ] || die "engine not found: $ENGINE (build it, or pass --engine)"
    local work
    work=$(mktemp -d)
    SESSION=$work/session
    WITNESS=$work/witness
    BACKEND_ARGS=(--backend fake)
    ALLOW_SAME_DEVICE=1 # one temp dir: the script, not the disk, is under test
    # The refusal itself is part of the proof.
    if (ALLOW_SAME_DEVICE=0; check_devices) 2>/dev/null; then
        echo "self-test FAIL: same-device witness was accepted"; rm -rf "$work"; return 1
    fi
    ARM_SECONDS=4 cmd_arm || { rm -rf "$work"; return 1; }
    kill -KILL "$ENGINE_PID" 2>/dev/null
    wait "$ENGINE_PID" 2>/dev/null
    ENGINE_PID=""
    cmd_verify
    local rc=$?
    rm -rf "$work"
    [ $rc = 0 ] && echo "self-test PASS"
    return $rc
}

MODE=""
while [ $# -gt 0 ]; do
    case $1 in
        arm|verify) MODE=$1 ;;
        --self-test) MODE=self-test ;;
        --session) SESSION=${2:?}; shift ;;
        --witness) WITNESS=${2:?}; shift ;;
        --tracks) TRACKS=${2:?}; shift ;;
        --loss-seconds) LOSS_SECONDS=${2:?}; shift ;;
        --engine) ENGINE=${2:?}; shift ;;
        *) die "unknown argument: $1" ;;
    esac
    shift
done

case $MODE in
    arm) cmd_arm ;;
    verify) cmd_verify ;;
    self-test) cmd_self_test ;;
    *) die "usage: arm|verify|--self-test (see the header)" ;;
esac
