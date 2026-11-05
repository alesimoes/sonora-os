#!/usr/bin/env bash
# The machine-wide heavy lock (owner, 2026-11-05: one heavy build at a time,
# using every CPU, the rest queued). Every build and test that fills the
# machine runs through this script, whatever shell or worktree it comes from.
#
#   tools/build-slot.sh COMMAND [ARG...]   queue, run when it is our turn
#   tools/build-slot.sh --jobs              the -j a build should use
#   tools/build-slot.sh --list              who holds the lock, who waits
#
# SONORA_HEAVY_PRIORITY orders the queue: 0 first (a PR's batch), 1 main,
# 2 anything else (default); first come first served within a priority.
# The capacity is one machine setting, SONORA_HEAVY_SLOTS=N in heavy.conf next
# to the lock (default 1), never a per-caller variable: callers that each
# chose their own number did not respect one another.
#
# The lock lives at a fixed place every caller reaches the same way: Git bash,
# WSL and PowerShell on this Windows host use %LOCALAPPDATA%\sonora\build-lock;
# a real Linux host uses /var/tmp/sonora-build-lock. (/tmp differs between
# Git bash, WSL and the containers, which is why it is not used.)
#
# A holder is a directory made with mkdir (atomic) whose files record the
# owner, the command and the start time, for the dashboard. A holder whose
# pid is dead is reaped (pids of another kind of shell cannot be probed: those
# are never reaped). A waiter is a ticket file it touches while it waits; a
# ticket nobody touched for 5 minutes is dropped. A command started from
# inside the lock (SONORA_HEAVY_HELD) runs at once, so a script may wrap its
# own steps and the scripts it calls may wrap them again. After
# SONORA_HEAVY_WAIT seconds (default 4 h) the command runs anyway, with a
# warning: a stuck queue must not stop work for good.
set -euo pipefail

host_kind() {
  case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*) echo win ;;
    *) echo linux ;;
  esac
}

# The lock directory. SONORA_HEAVY_DIR is for the tests only.
lock_dir() {
  if [[ -n "${SONORA_HEAVY_DIR:-}" ]]; then
    printf '%s' "$SONORA_HEAVY_DIR"
  elif [[ "$(host_kind)" == win ]]; then
    printf '%s/sonora/build-lock' "$(cygpath -u "${LOCALAPPDATA:-$USERPROFILE/AppData/Local}")"
  elif grep -qi microsoft /proc/version 2>/dev/null && command -v cmd.exe >/dev/null 2>&1 \
    && win="$(cmd.exe /c 'echo %LOCALAPPDATA%' 2>/dev/null | tr -d '\r')" && [[ -n "$win" ]]; then
    printf '%s/sonora/build-lock' "$(wslpath -u "$win")"
  else
    printf '%s' /var/tmp/sonora-build-lock
  fi
}

DIR="$(lock_dir)"
WAIT="${SONORA_HEAVY_WAIT:-14400}"
POLL="${SONORA_HEAVY_POLL:-2}"
PRIO="${SONORA_HEAVY_PRIORITY:-2}"
KIND="$(host_kind)"
export LC_ALL=C

slots() {
  local n=""
  [[ -f "$DIR/heavy.conf" ]] && n="$(sed -n 's/^[[:space:]]*SONORA_HEAVY_SLOTS=\([0-9]*\).*/\1/p' "$DIR/heavy.conf" | tail -n 1)"
  [[ "$n" =~ ^[1-9][0-9]*$ ]] || n=1
  echo "$n"
}

if [[ "${1:-}" == --jobs ]]; then
  cpus=""
  if command -v docker >/dev/null 2>&1; then
    cpus="$(docker info --format '{{.NCPU}}' 2>/dev/null || true)"
  fi
  [[ "$cpus" =~ ^[0-9]+$ && "$cpus" -gt 0 ]] || cpus="$(nproc 2>/dev/null || echo 4)"
  jobs=$((cpus / $(slots)))
  ((jobs >= 2)) || jobs=2
  echo "$jobs"
  exit 0
fi

if [[ "${1:-}" == --list ]]; then
  for d in "$DIR"/slot-*; do
    [[ -f "$d/owner" ]] && echo "holds $(tr '\n' ' ' <"$d/owner")"
  done
  for t in "$DIR"/wait/*; do
    [[ -f "$t" ]] && echo "waits $(tr '\n' ' ' <"$t")"
  done
  exit 0
fi

[[ $# -gt 0 ]] || { echo "usage: build-slot.sh COMMAND [ARG...] | --jobs | --list" >&2; exit 2; }
[[ -z "${SONORA_HEAVY_HELD:-}" ]] || exec "$@"

mkdir -p "$DIR/wait"
ticket="$DIR/wait/$PRIO-$(printf '%020d' "$(date +%s%N)")-$$"
held=""
# shellcheck disable=SC2329  # runs from the EXIT trap
release() {
  rm -f "$ticket"
  if [[ -n "$held" ]]; then
    rm -rf "$held"
    held=""
  fi
}
trap release EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# What the dashboard shows for a holder or a waiter.
describe() {
  printf 'pid=%s\nkind=%s\nuser=%s\nprio=%s\nstart=%s\ncwd=%s\ncmd=%s\n' \
    "$$" "$KIND" "${USERNAME:-${USER:-?}}" "$PRIO" "$(date +%s)" "$PWD" "$(printf '%s ' "$@" | cut -c1-200)"
}

alive() {  # a pid of another kind of shell cannot be probed: assume alive
  local k pid
  k="$(sed -n 's/^kind=//p' "$1" 2>/dev/null)"
  pid="$(sed -n 's/^pid=//p' "$1" 2>/dev/null)"
  [[ "$pid" =~ ^[0-9]+$ ]] || return 1
  [[ "$k" != "$KIND" ]] || kill -0 "$pid" 2>/dev/null
}

# A directory with no owner file yet is a claim between mkdir and the write;
# only when it stays that way for a minute was its owner killed in between.
reap() {
  local d t
  for d in "$DIR"/slot-*; do
    [[ -d "$d" ]] || continue
    if [[ -f "$d/owner" ]]; then
      alive "$d/owner" || rm -rf "$d"
    elif [[ -n "$(find "$d" -maxdepth 0 -mmin +1 2>/dev/null)" ]]; then
      rm -rf "$d"
    fi
  done
  for t in "$DIR"/wait/*; do
    [[ -f "$t" ]] || continue
    if ! alive "$t" || [[ -n "$(find "$t" -mmin +5 2>/dev/null)" ]]; then
      rm -f "$t"
    fi
  done
}

# Free capacity, and how many waiters sit ahead of us in the order
# (priority, then arrival: the ticket names sort that way).
may_go() {
  local d t used=0 ahead=0
  for d in "$DIR"/slot-*; do [[ -d "$d" ]] && used=$((used + 1)); done
  for t in "$DIR"/wait/*; do
    [[ -f "$t" && "$t" < "$ticket" ]] && ahead=$((ahead + 1))
  done
  ((ahead + used < $(slots)))
}

take() {
  local i n
  n="$(slots)"
  for ((i = 0; i < n; i++)); do
    if mkdir "$DIR/slot-$i" 2>/dev/null; then
      held="$DIR/slot-$i"
      describe "$@" >"$held/owner"
      return 0
    fi
  done
  return 1
}

describe "$@" >"$ticket"
start=$SECONDS
announced=0
reap
until may_go && take "$@"; do
  if ((announced == 0)); then
    echo "waiting for the heavy lock (priority $PRIO): $("$0" --list | tr '\n' ';')" >&2
    announced=1
  fi
  if ((SECONDS - start >= WAIT)); then
    echo "warning: no heavy lock after ${WAIT}s; running anyway" >&2
    break
  fi
  sleep "$POLL"
  touch "$ticket"
  reap
done
rm -f "$ticket"

export SONORA_HEAVY_HELD=1
rc=0
"$@" || rc=$?
exit "$rc"
