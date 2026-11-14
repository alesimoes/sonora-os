#!/usr/bin/env bash
# A heavy command run detached under the machine lock, for a build that is
# longer than the caller's command time limit (an agent's tool call is cut at
# about ten minutes; a UI micro build plus its wait for the lock can be
# longer). The command still goes through tools/build-slot.sh, so it still
# waits its turn and still runs alone; only the waiting moves out of the
# caller's call.
#
#   tools/build-job.sh start COMMAND [ARG...]   queue + run detached; prints the id
#   tools/build-job.sh wait [ID] [--max S]      block up to S s (default 540) for it
#   tools/build-job.sh cancel [ID]              stop it (its container too)
#   tools/build-job.sh list                     this checkout's jobs
#
# tools/build-slot.sh --detach is `start`; tools/build-wait.sh is `wait`.
# `wait` exits with the command's status once it ended, and 75 when it is
# still queued or running after S seconds: call it again, nothing is lost.
# 70: the job's runner died without a result (killed); start it again.
# One unfinished job per checkout at a time: a second `start` names the
# running one and exits 3 (never two builds of one tree).
#
# A job is a directory under .build-jobs/ in the checkout: cmd, cwd, log,
# pid/winpid of its runner, queued (seconds it waited for the lock), rc.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
JOBS_DIR="${SONORA_BUILD_JOBS_DIR:-$ROOT/.build-jobs}"
SLOT="$ROOT/tools/build-slot.sh"
POLL="${SONORA_BUILD_WAIT_POLL:-5}"
KEEP=10

usage() {
  sed -n '9,12p' "${BASH_SOURCE[0]}" | sed 's/^# *//' >&2
  exit 2
}

python_bin() {
  local p
  for p in python3 python py; do
    # The Windows Store's python3 stub exists but runs nothing.
    if command -v "$p" >/dev/null 2>&1 && "$p" -c 'import sys' >/dev/null 2>&1; then
      echo "$p"; return 0
    fi
  done
  return 1
}

job_ids() {  # oldest first: the ids start with their start time
  local d
  for d in "$JOBS_DIR"/*/; do
    [[ -d "$d" ]] || continue
    d="${d%/}"
    echo "${d##*/}"
  done | sort
}

job_dir() {  # [ID]: the job's directory; the newest when no id is given
  local id="${1:-}"
  if [[ -z "$id" ]]; then
    id="$(job_ids | tail -n 1)"
    [[ -n "$id" ]] || { echo "build-job: no job in $JOBS_DIR" >&2; return 1; }
  fi
  [[ -d "$JOBS_DIR/$id" ]] || { echo "build-job: no job $id in $JOBS_DIR" >&2; return 1; }
  printf '%s' "$JOBS_DIR/$id"
}

runner_alive() {
  local pid
  pid="$(cat "$1/pid" 2>/dev/null)" || return 1
  [[ "$pid" =~ ^[0-9]+$ ]] && kill -0 "$pid" 2>/dev/null
}

# done | queued | running | starting | lost
state() {
  local d="$1"
  if [[ -f "$d/rc" ]]; then echo "done"
  elif [[ ! -f "$d/pid" ]]; then
    if [[ -n "$(find "$d/cmd" -mmin -1 2>/dev/null)" ]]; then echo starting; else echo lost; fi
  elif runner_alive "$d"; then
    if [[ -f "$d/queued" ]]; then echo running; else echo queued; fi
  elif [[ -f "$d/rc" ]]; then echo "done"   # it ended between the two looks
  else echo lost
  fi
}

since() { echo $(($(date +%s) - $(cat "$1" 2>/dev/null || date +%s))); }

# ---------------------------------------------------------------------------
cmd_start() {
  [[ $# -gt 0 ]] || usage
  local d id py bash_exe pid i
  mkdir -p "$JOBS_DIR"
  for d in "$JOBS_DIR"/*/; do
    [[ -d "$d" ]] || continue
    d="${d%/}"
    case "$(state "$d")" in
      queued | running | starting)
        echo "build-job: a job is already running in this checkout: ${d##*/}" >&2
        echo "build-job: collect it with tools/build-wait.sh ${d##*/} (or tools/build-job.sh cancel ${d##*/})" >&2
        exit 3 ;;
    esac
  done
  py="$(python_bin)" || { echo "build-job: python is required" >&2; exit 1; }
  id="$(date +%Y%m%d-%H%M%S)-$$"
  d="$JOBS_DIR/$id"
  mkdir -p "$d"
  printf '%s\0' "$@" >"$d/cmd"
  printf '%s' "$PWD" >"$d/cwd"
  bash_exe="$BASH"
  if command -v cygpath >/dev/null 2>&1; then bash_exe="$(cygpath -w "$BASH")"; fi
  # The job must not think it runs inside its caller's lock.
  if ! env -u SONORA_HEAVY_HELD -u SONORA_HEAVY_SLOT_DIR "$py" "$ROOT/tools/harness/detach.py" \
      --cwd "$PWD" --out "$d/runner.out" -- "$bash_exe" "$ROOT/tools/build-job.sh" run "$d" >/dev/null; then
    echo "build-job: could not start the runner" >&2
    exit 1
  fi
  for ((i = 0; i < 100; i++)); do
    [[ ! -f "$d/pid" ]] || break
    sleep 0.1
  done
  pid="$(cat "$d/pid" 2>/dev/null || true)"
  [[ -n "$pid" ]] || { echo "build-job: the runner did not start:" >&2; cat "$d/runner.out" >&2; exit 1; }
  prune
  echo "build-job: started $id: $*"
  echo "build-job: log $d/log"
  echo "build-job: collect the result with: tools/build-wait.sh $id"
}

prune() {  # keep the newest KEEP finished jobs
  local old
  job_ids | head -n -"$KEEP" | while IFS= read -r old; do
    [[ -f "$JOBS_DIR/$old/rc" ]] && rm -rf "${JOBS_DIR:?}/$old"
  done
}

# shellcheck disable=SC2329  # runs from cmd_run's trap
stop_containers() {
  local c
  [[ -f "$1/containers" ]] || return 0
  while IFS= read -r c; do
    [[ -z "$c" ]] || timeout 60 docker kill "$c" >/dev/null 2>&1 || true
  done <"$1/containers"
}

# The detached side: run the command through the lock, record the result.
cmd_run() {
  local d="$1" child rc=0 cmd=()
  echo "$$" >"$d/pid.tmp" && mv "$d/pid.tmp" "$d/pid"
  if [[ -r "/proc/$$/winpid" ]]; then cat "/proc/$$/winpid" >"$d/winpid"; fi
  date +%s >"$d/started"
  mapfile -d '' cmd <"$d/cmd"
  cd "$(cat "$d/cwd")" || { echo 2 >"$d/rc"; exit 2; }
  SONORA_HEAVY_WAIT_FILE="$d/queued" SONORA_HEAVY_CONTAINER_LOG="$d/containers" \
    "$SLOT" "${cmd[@]}" >>"$d/log" 2>&1 </dev/null &
  child=$!
  # cancel: the lock script's trap only runs once its docker client ended,
  # so the containers are stopped here first; the lock is then released.
  trap 'stop_containers "$d"; kill -TERM "$child" 2>/dev/null' TERM INT
  wait "$child" || rc=$?
  while kill -0 "$child" 2>/dev/null; do
    rc=0
    wait "$child" || rc=$?
  done
  date +%s >"$d/ended"
  echo "$rc" >"$d/rc.tmp" && mv "$d/rc.tmp" "$d/rc"
}

report() {  # DIR STATE: one status line, and the log's tail when it ended
  local d="$1" st="$2" tail_n="${3:-40}"
  local id="${d##*/}" q
  q="$(cat "$d/queued" 2>/dev/null || echo "")"
  case "$st" in
    done)
      echo "build-job: $id finished: exit $(cat "$d/rc"), $(($(cat "$d/ended") - $(cat "$d/started"))) s" \
        "(${q:-0} s of it waiting for the lock); log $d/log"
      echo "--- last $tail_n lines of the log"
      tail -n "$tail_n" "$d/log" ;;
    queued)
      echo "build-job: $id is waiting for the heavy lock ($(since "$d/started") s so far):"
      "$SLOT" --list | sed 's/^/  /' ;;
    running)
      echo "build-job: $id is running ($(($(since "$d/started") - ${q:-0})) s so far, after ${q:-0} s in the queue); last lines:"
      tail -n 5 "$d/log" 2>/dev/null | sed 's/^/  /' ;;
    starting) echo "build-job: $id is starting" ;;
    lost)
      echo "build-job: $id has no result and its runner is gone (killed?); log $d/log"
      tail -n 20 "$d/log" 2>/dev/null ;;
  esac
}

cmd_wait() {
  local id="" max="${SONORA_BUILD_WAIT_MAX:-540}" tail_n=40 d st start=$SECONDS
  while (($#)); do
    case "$1" in
      --max) max="$2"; shift ;;
      --tail) tail_n="$2"; shift ;;
      -*) usage ;;
      *) id="$1" ;;
    esac
    shift
  done
  d="$(job_dir "$id")" || exit 2
  while :; do
    st="$(state "$d")"
    case "$st" in
      done) report "$d" "done" "$tail_n"; exit "$(cat "$d/rc")" ;;
      lost) report "$d" lost; exit 70 ;;
    esac
    if ((SECONDS - start >= max)); then
      report "$d" "$st"
      echo "build-job: not finished; call tools/build-wait.sh ${d##*/} again (exit 75)"
      exit 75
    fi
    sleep "$POLL"
  done
}

cmd_cancel() {
  local d pid i
  d="$(job_dir "${1:-}")" || exit 2
  pid="$(cat "$d/pid" 2>/dev/null || true)"
  if [[ "$pid" =~ ^[0-9]+$ ]] && runner_alive "$d"; then
    kill -TERM "$pid" 2>/dev/null || true
    for ((i = 0; i < 90; i++)); do
      [[ ! -f "$d/rc" ]] || break
      sleep 1
    done
  fi
  report "$d" "$(state "$d")"
}

cmd_list() {
  local d
  for d in "$JOBS_DIR"/*/; do
    [[ -d "$d" ]] || continue
    d="${d%/}"
    printf '%s %s %s\n' "${d##*/}" "$(state "$d")" "$(tr '\0' ' ' <"$d/cmd" | cut -c1-120)"
  done
}

sub="${1:-}"
shift || true
case "$sub" in
  start) cmd_start "$@" ;;
  run) cmd_run "$@" ;;
  wait) cmd_wait "$@" ;;
  cancel) cmd_cancel "$@" ;;
  list) cmd_list ;;
  *) usage ;;
esac
