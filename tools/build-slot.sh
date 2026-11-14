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
#
# A `docker run` command gets a container name (sonora-heavy-PID-TIME unless
# it has its own --name), recorded in the owner file. Its slot stays held
# while that container runs, even when the client is dead: an agent's tool
# call cut at its time limit kills the client, never the container, which
# kept building outside the lock while the next build took the slot
# (2026-11-09: ten builds at once, Docker hung, every agent build timed out).
# A holder stopped by a signal kills its container on the way out.
#
# A holder records its Windows pid too (winpid, Git bash only): the bash pid
# ($$) is an MSYS number the Windows-side supervisor cannot probe, and taking
# it for a Windows pid made it reap live holders, so a second build started
# beside the first (2026-11-13: 430 holders reaped as "pid dead", builds
# still running among them). A holder only ever removes its own slot
# directory, so one reaped by mistake does not free the next holder's slot.
# A docker run nested in a held command (micro-build.sh under
# `build-slot.sh tools/micro-build.sh`) is named too and added to the
# holder's owner file (SONORA_HEAVY_SLOT_DIR): the slot stays held while it
# runs, and a stopped holder kills it.
#
#   tools/build-slot.sh --detach COMMAND [ARG...]
# queues and runs the command detached from the caller (tools/build-job.sh),
# for a build longer than the caller's command time limit; the caller then
# collects the result with tools/build-wait.sh, which returns within that
# limit and may be called again.
#
# SONORA_HEAVY_WAIT_FILE=PATH: the seconds spent waiting for the lock are
# written there once it is ours (tools/micro-build.sh logs lock waits apart from
# step times). The variable is not passed on to the command, so a nested call
# does not overwrite it with its own zero.
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

usage() { echo "usage: build-slot.sh [--detach] COMMAND [ARG...] | --jobs | --list" >&2; exit 2; }
if [[ "${1:-}" == --detach ]]; then
  shift
  [[ $# -gt 0 ]] || usage
  exec "$(dirname "${BASH_SOURCE[0]}")/build-job.sh" start "$@"
fi
[[ $# -gt 0 ]] || usage
# A flag this script does not know is a typo, never a command: `--status`
# once sat in the queue as a job (2026-11-07).
[[ "$1" != -* ]] || { echo "build-slot.sh: unknown flag $1" >&2; usage; }
WAIT_FILE="${SONORA_HEAVY_WAIT_FILE:-}"
unset SONORA_HEAVY_WAIT_FILE
# SONORA_HEAVY_EXIT_FILE=PATH: our own exit status, written on the way out by
# the EXIT trap. A process killed from outside (the supervisor's taskkill, a
# tool call's tree kill) never writes it, and under Git bash its parent's
# `wait` reports that kill as 0: tools/build-job.sh took it for a success
# (2026-11-14: waiters "finished: exit 0" with only the lock-wait line, no
# build run). Not passed on to the command.
EXIT_FILE="${SONORA_HEAVY_EXIT_FILE:-}"
unset SONORA_HEAVY_EXIT_FILE

# The Windows pid of this Git bash, which the supervisor (Windows Python)
# can probe; nothing elsewhere.
winpid() {
  if [[ "$KIND" == win && -r "/proc/$$/winpid" ]]; then cat "/proc/$$/winpid" 2>/dev/null || true; fi
}

# The last container an owner file names (a nested run appends its own).
owner_container() {
  sed -n 's/^container=//p' "$1" 2>/dev/null | tail -n 1
}

# The slot directory still records this process: a holder reaped by mistake
# must not remove the slot the next holder took since.
# shellcheck disable=SC2329  # runs from the EXIT trap
ours() {
  [[ -f "$1/owner" ]] && grep -qx "pid=$$" "$1/owner" && grep -qx "kind=$KIND" "$1/owner"
}

ticket="" held="" container="" running=0
# shellcheck disable=SC2329  # runs from the EXIT trap
release() {
  local rc=$?
  if [[ -n "$EXIT_FILE" ]]; then echo "$rc" >"$EXIT_FILE.tmp" && mv "$EXIT_FILE.tmp" "$EXIT_FILE"; fi
  [[ -z "$ticket" ]] || rm -f "$ticket"
  if ((running)); then
    local c="$container"
    [[ -z "$held" ]] || c="$(owner_container "$held/owner")"
    [[ -z "$c" ]] || timeout 60 docker kill "$c" >/dev/null 2>&1 || true
  fi
  if [[ -n "$held" ]]; then
    if ours "$held"; then rm -rf "$held"; fi
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
  local w
  w="$(winpid)"
  [[ -z "$w" ]] || printf 'winpid=%s\n' "$w"
  [[ -z "$container" ]] || printf 'container=%s\n' "$container"
}

# The holder's container still runs. A docker that does not answer in time
# counts as running: a hung daemon is a loaded machine, not a free one.
container_running() {
  local c state rc=0
  c="$(owner_container "$1")"
  [[ -n "$c" ]] || return 1
  state="$(timeout 30 docker inspect -f '{{.State.Running}}' "$c" 2>/dev/null)" || rc=$?
  ((rc != 124)) || return 0
  [[ "$state" == true ]]
}

alive() {  # a pid of another kind of shell cannot be probed: assume alive
  local k pid
  k="$(sed -n 's/^kind=//p' "$1" 2>/dev/null)"
  pid="$(sed -n 's/^pid=//p' "$1" 2>/dev/null)"
  [[ "$pid" =~ ^[0-9]+$ ]] || return 1
  if [[ "$k" == "$KIND" ]] && ! kill -0 "$pid" 2>/dev/null; then
    container_running "$1"
  fi
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

cmd=("$@")
if [[ "${1##*/}" == docker || "${1##*/}" == docker.exe ]] && [[ "${2:-}" == run ]]; then
  prev=""
  for a in "${@:3}"; do
    case "$a" in --name=*) container="${a#--name=}" ;; esac
    [[ "$prev" != --name ]] || container="$a"
    prev="$a"
  done
  if [[ -z "$container" ]]; then
    container="sonora-heavy-$$-$(date +%s)"
    cmd=("$1" run --name "$container" "${@:3}")
  fi
fi
# A detached job (tools/build-job.sh) lists its containers, to stop them
# when it is cancelled: a signalled holder waits for its docker client.
if [[ -n "$container" && -n "${SONORA_HEAVY_CONTAINER_LOG:-}" ]]; then
  echo "$container" >>"$SONORA_HEAVY_CONTAINER_LOG"
fi

# Inside the lock already: run at once. A nested docker run is added to the
# holder's owner file, so the slot stays held while it runs, and it is
# killed if this call is stopped.
if [[ -n "${SONORA_HEAVY_HELD:-}" ]]; then
  [[ -n "$container" ]] || exec "$@"
  slot="${SONORA_HEAVY_SLOT_DIR:-}"
  if [[ -n "$slot" && -f "$slot/owner" ]]; then printf 'container=%s\n' "$container" >>"$slot/owner"; fi
  rc=0
  running=1
  "${cmd[@]}" || rc=$?
  running=0
  exit "$rc"
fi

mkdir -p "$DIR/wait"
ticket="$DIR/wait/$PRIO-$(printf '%020d' "$(date +%s%N)")-$$"
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
  # A ticket another reaper removed is written again whole: an empty one
  # names no pid and is dropped as dead at the next look.
  if [[ -s "$ticket" ]]; then touch "$ticket"; else describe "$@" >"$ticket"; fi
  reap
done
rm -f "$ticket"
[[ -z "$WAIT_FILE" ]] || echo "$((SECONDS - start))" >"$WAIT_FILE"

export SONORA_HEAVY_HELD=1
[[ -z "$held" ]] || export SONORA_HEAVY_SLOT_DIR="$held"
rc=0
running=1
"${cmd[@]}" || rc=$?
running=0
exit "$rc"
