#!/usr/bin/env bash
# Global build slot limiter, host side. Many agents build at once, each in its
# own container; without a limit every build believed it owned all the CPUs
# and the host ran at 90% load. At most SONORA_BUILD_SLOTS (default 4)
# commands run through this script at the same time, across all worktrees.
#
#   tools/build-slot.sh COMMAND [ARG...]   wait for a slot, run, release
#   tools/build-slot.sh --jobs              the -j each build should use:
#                                           the CPUs Docker has / slots, min 2
#
# A slot is a directory made with mkdir (atomic) holding the owner's pid. A
# slot whose pid is dead is reaped. After SONORA_BUILD_SLOT_WAIT seconds
# (default 1800) the command runs anyway, with a warning: a stuck queue must
# not stop work for good.
set -euo pipefail

SLOTS="${SONORA_BUILD_SLOTS:-4}"
WAIT="${SONORA_BUILD_SLOT_WAIT:-1800}"
POLL="${SONORA_BUILD_SLOT_POLL:-2}"
DIR="${TMPDIR:-/tmp}/sonora-build-slots"

[[ "$SLOTS" =~ ^[1-9][0-9]*$ ]] || { echo "error: SONORA_BUILD_SLOTS must be a positive integer" >&2; exit 2; }

if [[ "${1:-}" == --jobs ]]; then
  cpus=""
  if command -v docker >/dev/null 2>&1; then
    cpus="$(docker info --format '{{.NCPU}}' 2>/dev/null || true)"
  fi
  [[ "$cpus" =~ ^[0-9]+$ && "$cpus" -gt 0 ]] || cpus="$(nproc 2>/dev/null || echo 4)"
  jobs=$((cpus / SLOTS))
  ((jobs >= 2)) || jobs=2
  echo "$jobs"
  exit 0
fi

[[ $# -gt 0 ]] || { echo "usage: build-slot.sh COMMAND [ARG...] | --jobs" >&2; exit 2; }
mkdir -p "$DIR"

held=""
# shellcheck disable=SC2329  # runs from the EXIT trap
release() {
  rm -f "$DIR/wait-$$"
  if [[ -n "$held" ]]; then
    rm -rf "$held"
    held=""
  fi
}
trap release EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# A slot is stale when its owner is gone. A directory with no pid yet is a
# claim between mkdir and the pid write; only when it stays that way for a
# minute was its owner killed in between.
reap() {
  local d pid
  for d in "$DIR"/slot-*; do
    [[ -d "$d" ]] || continue
    if [[ -f "$d/pid" ]]; then
      pid="$(cat "$d/pid" 2>/dev/null || true)"
      if [[ ! "$pid" =~ ^[0-9]+$ ]] || ! kill -0 "$pid" 2>/dev/null; then
        rm -rf "$d"
      fi
    elif [[ -n "$(find "$d" -maxdepth 0 -mmin +1 2>/dev/null)" ]]; then
      rm -rf "$d"
    fi
  done
}

try_acquire() {
  local i
  for ((i = 0; i < SLOTS; i++)); do
    if mkdir "$DIR/slot-$i" 2>/dev/null; then
      held="$DIR/slot-$i"
      echo "$$" >"$held/pid"
      return 0
    fi
  done
  return 1
}

# Waiters ahead of us, by the age of their marker files, plus one.
position() {
  local n=1 w
  for w in "$DIR"/wait-*; do
    [[ -e "$w" && "$w" != "$DIR/wait-$$" && ! "$w" -nt "$DIR/wait-$$" ]] && n=$((n + 1))
  done
  echo "$n"
}

start=$SECONDS
announced=0
reap
until try_acquire; do
  if ((announced == 0)); then
    : >"$DIR/wait-$$"
    echo "waiting for a build slot (position $(position))" >&2
    announced=1
  fi
  if ((SECONDS - start >= WAIT)); then
    echo "warning: no build slot after ${WAIT}s; running anyway" >&2
    break
  fi
  sleep "$POLL"
  reap
done
rm -f "$DIR/wait-$$"

rc=0
"$@" || rc=$?
exit "$rc"
