#!/usr/bin/env bash
# Run the whole DAW on this machine: the engine and an interface as two
# processes (ADR-0007 §3), the way the appliance runs them. For a Linux
# desktop or WSL; nothing here touches the ISO.
#
#   tools/run-sonora.sh                     # engine + Qt UI
#   tools/run-sonora.sh --backend fake      # no audio server needed
#   tools/run-sonora.sh --ui tui            # the terminal interface instead
#   tools/run-sonora.sh -- --demo           # anything after -- goes to the UI
#
# Backends: jack, which on PipeWire runs through pw-jack; fake, an 18-input
# UMC1820 stand-in with a distinct tone per input and no audio server at
# all. The default is jack when a PipeWire server answers, fake otherwise.
#
# The binaries come from a native build (cmake -S . -B build): the
# container builds (tools/build-engine.sh, tools/build-ui.sh) are for
# Debian trixie, not for this host. SONORA_BUILD names another directory.
#
# The engine wants unlimited locked memory, as the appliance grants it.
# Where the limit is lower (WSL's default is 64 KiB) it runs with
# --no-memlock and says so: fine for trying the DAW, not for judging xruns.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${SONORA_BUILD:-$ROOT/build}"

backend=auto
ui=qt
dir="$HOME/Sessions/Untitled"
tracks=auto
ui_args=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --backend) backend="$2"; shift 2 ;;
    --ui) ui="$2"; shift 2 ;;
    --dir) dir="$2"; shift 2 ;;
    --tracks) tracks="$2"; shift 2 ;;
    --) shift; ui_args=("$@"); break ;;
    -h|--help) sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "run-sonora: unknown option $1 (see --help)" >&2; exit 2 ;;
  esac
done

ENGINE="$BUILD/sonora-engine"
case "$ui" in
  qt) UI="$BUILD/sonora-ui" ;;
  tui) UI="$BUILD/sonora-tui" ;;
  *) echo "run-sonora: --ui is qt or tui" >&2; exit 2 ;;
esac
for bin in "$ENGINE" "$UI"; do
  [[ -x "$bin" ]] || {
    echo "run-sonora: $bin is missing. Build natively first:" >&2
    echo "  cmake -S \"$ROOT\" -B \"$BUILD\" && cmake --build \"$BUILD\" -j" >&2
    echo "  (sonora-ui needs Qt 6.7 or later: qt6-declarative-dev and friends)" >&2
    exit 1
  }
done

if [[ "$backend" == auto ]]; then
  if command -v pw-cli >/dev/null && pw-cli info 0 >/dev/null 2>&1; then
    backend=jack
  else
    backend=fake
    echo "run-sonora: no PipeWire server answers; using the fake backend"
  fi
fi

engine_cmd=("$ENGINE" --backend "$backend" --dir "$dir" --tracks "$tracks" --headless)
if [[ "$backend" == jack ]] && command -v pw-jack >/dev/null; then
  # PipeWire's libjack, whichever libjack the binary was linked against.
  engine_cmd=(pw-jack "${engine_cmd[@]}")
fi
# Extra engine flags from a wrapper (tools/dev-wsl.sh: --no-input-monitoring).
if [[ -n "${SONORA_ENGINE_ARGS:-}" ]]; then
  read -r -a extra <<<"$SONORA_ENGINE_ARGS"
  engine_cmd+=("${extra[@]}")
fi
if [[ "$(ulimit -l)" != unlimited ]]; then
  echo "run-sonora: locked memory is limited to $(ulimit -l) KiB here; running the" \
       "engine with --no-memlock (xruns are possible; never so on the appliance)"
  engine_cmd+=(--no-memlock)
fi

runtime="${XDG_RUNTIME_DIR:-}"
if [[ -n "$runtime" ]]; then
  sock="$runtime/sonora/engine.sock"
else
  sock="/tmp/sonora-$(id -u)/engine.sock"
fi
engine_cmd+=(--control-socket "$sock")

logs="$(mktemp -d "${TMPDIR:-/tmp}/sonora-run.XXXXXX")"
echo "run-sonora: engine ($backend) -> $dir; logs in $logs"
"${engine_cmd[@]}" >"$logs/engine.out" 2>"$logs/engine.err" &
engine_pid=$!

stop_engine() {
  if kill -0 "$engine_pid" 2>/dev/null; then
    # SIGTERM closes any open take cleanly, as a quit key does.
    kill -TERM "$engine_pid" 2>/dev/null || true
    wait "$engine_pid" 2>/dev/null || true
  fi
}
trap stop_engine EXIT INT TERM

for _ in $(seq 1 150); do
  [[ -S "$sock" ]] && break
  if ! kill -0 "$engine_pid" 2>/dev/null; then
    echo "run-sonora: the engine exited before listening:" >&2
    cat "$logs/engine.out" "$logs/engine.err" >&2
    exit 1
  fi
  sleep 0.2
done
[[ -S "$sock" ]] || { echo "run-sonora: the engine never opened $sock" >&2; exit 1; }
echo "run-sonora: engine listening on $sock (pid $engine_pid)"

if [[ "$ui" == qt ]]; then
  ui_cmd=("$UI" --engine-socket "$sock" "${ui_args[@]}")
else
  ui_cmd=("$UI" --control-socket "$sock" "${ui_args[@]}")
fi
set +e
"${ui_cmd[@]}"
ui_rc=$?
set -e
echo "run-sonora: the interface exited ($ui_rc); stopping the engine"
stop_engine
trap - EXIT INT TERM
grep -h "^sonora: session written" "$logs/engine.out" || true
exit "$ui_rc"
