#!/usr/bin/env bash
# The DAW on the owner's Windows laptop: Debian trixie under WSL2, the window
# through WSLg, the sound through WSLg's PulseAudio server to the Windows
# speakers and from the Windows microphone. Run inside the Debian distro,
# from the checkout (tools/dev-wsl.ps1 does that from Windows):
#
#   tools/dev-wsl.sh            deps, build, then engine + Qt UI   (default)
#   tools/dev-wsl.sh deps       install the build and runtime packages (sudo)
#   tools/dev-wsl.sh build      configure and build engine, UI, sonora-ctl
#   tools/dev-wsl.sh audio      start PipeWire bridged to WSLg, then exit
#   tools/dev-wsl.sh run [-- UI ARGS]   engine + Qt UI (no deps, no build)
#   tools/dev-wsl.sh selftest [--tone] [SECONDS]
#                               record SECONDS (default 4) from the mic, or
#                               from a test tone, play it back to the
#                               speakers, and print the levels of the take
#                               and of what reached the speakers
#
# Every step is idempotent: packages already installed are not reinstalled,
# the build is incremental, a PipeWire already running is reused.
#
# This is the development path ADR-0007 M7 and ADR-0008 allow, not the
# appliance: the engine runs with --no-memlock (WSL's locked-memory limit is
# 64 KiB), there is no RT scheduling, and the sound goes through PipeWire ->
# pulse-tunnel -> WSLg's PulseAudio -> RDP audio -> Windows. Xruns and
# latency measured here say nothing about the appliance
# (planning/progress.json, devenv.wsl_measure).
#
# Environment:
#   SONORA_BUILD     build directory (default ~/.cache/sonora/build-wsl, on
#                    the distro's own disk: building on /mnt/c is several
#                    times slower)
#   SONORA_DIR       session directory (default ~/Sessions/Untitled)
#   SONORA_MONITOR=1 let armed tracks monitor their input. Off by default:
#                    a laptop's own mic and speakers feed back.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${SONORA_BUILD:-$HOME/.cache/sonora/build-wsl}"
SESSION_DIR="${SONORA_DIR:-$HOME/Sessions/Untitled}"
WSLG_PULSE=/mnt/wslg/PulseServer
SPEAKERS=wslg-speakers
MIC=wslg-mic

say() { echo "dev-wsl: $*"; }
die() { echo "dev-wsl: $*" >&2; exit 1; }

# --- checks ----------------------------------------------------------------

check_host() {
  [[ -n "${WSL_DISTRO_NAME:-}" ]] || grep -qi microsoft /proc/version ||
    die "this is not WSL. On a Linux desktop use tools/run-sonora.sh."
  # shellcheck source=/dev/null
  . /etc/os-release
  # The container images and the ISO are trixie. Another release builds
  # against another Qt and another PipeWire, and a result there is not one
  # about what we ship.
  [[ "${VERSION_CODENAME:-}" == trixie ]] ||
    die "Debian ${VERSION_CODENAME:-?} here; trixie is required. Upgrade the distro:" \
        "sudo sed -i 's/bookworm/trixie/g' /etc/apt/sources.list" \
        "/etc/apt/sources.list.d/*.sources 2>/dev/null;" \
        "sudo apt update && sudo apt full-upgrade"
}

check_wslg() {
  [[ -S "$WSLG_PULSE" ]] ||
    die "$WSLG_PULSE is missing: WSLg is not running. Update WSL from Windows" \
        "(wsl --update) and reopen the distro; WSLg needs WSL from the Store."
  [[ -n "${WAYLAND_DISPLAY:-}${DISPLAY:-}" ]] ||
    die "no WAYLAND_DISPLAY or DISPLAY: WSLg's display is not available."
}

# --- deps ------------------------------------------------------------------

# Matches tools/Dockerfile.build and tools/Dockerfile.ui, minus the headless
# display (Xvfb, cage, grim): WSLg is the display. libqt6opengl6-dev is a
# virtual package qt6-base-dev provides, so it is not listed. pipewire-audio and
# pipewire-pulse are left out on purpose: pipewire-pulse would serve a second
# PulseAudio socket beside WSLg's, and the bridge here goes the other way, a
# pulse-tunnel from PipeWire into WSLg's server (libpulse0 is what the
# tunnel module loads).
PACKAGES=(
  build-essential cmake pkg-config ca-certificates locales
  libjack-jackd2-dev
  qt6-base-dev qt6-declarative-dev qt6-declarative-dev-tools
  qml6-module-qtquick qml6-module-qtquick-window qml6-module-qtquick-layouts
  qml6-module-qtquick-controls qt6-wayland
  libgl1-mesa-dri libglx-mesa0 libegl-mesa0 fonts-inter
  pipewire pipewire-bin pipewire-jack libpipewire-0.3-modules libspa-0.2-modules
  wireplumber libpulse0 pulseaudio-utils
  dbus dbus-x11 python3 procps
)

do_deps() {
  check_host
  local missing=()
  for p in "${PACKAGES[@]}"; do
    dpkg-query -W -f='${Status}' "$p" 2>/dev/null | grep -q "install ok installed" ||
      missing+=("$p")
  done
  if ((${#missing[@]})); then
    say "installing ${#missing[@]} packages: ${missing[*]}"
    sudo apt-get update
    sudo DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${missing[@]}"
  else
    say "packages: all ${#PACKAGES[@]} installed"
  fi
  # The same drop-in the image installs: pipewire-jack's libjack must win
  # over libjack-jackd2-0 (pulled in by the -dev package), or the engine
  # looks for a jackd that is not running.
  local conf=/etc/ld.so.conf.d/pipewire-jack.conf
  if [[ ! -f "$conf" ]]; then
    sudo install -D -m 0644 \
      /usr/share/doc/pipewire/examples/ld.so.conf.d/pipewire-jack-x86_64-linux-gnu.conf "$conf"
    sudo ldconfig
  fi
}

# --- build -----------------------------------------------------------------

do_build() {
  check_host
  command -v cmake >/dev/null || die "cmake is missing; run: tools/dev-wsl.sh deps"
  cmake -S "$ROOT" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
  grep -q "^Qt6_DIR:PATH=/" "$BUILD/CMakeCache.txt" ||
    die "CMake did not find Qt 6.7+; run: tools/dev-wsl.sh deps"
  # Only what is run here; the full tree and ctest are tools/build-engine.sh
  # and tools/build-ui.sh.
  cmake --build "$BUILD" -j"$(nproc)" --target sonora-engine sonora-ui sonora-ctl sonora-tui
  say "built into $BUILD"
}

# --- audio -----------------------------------------------------------------

runtime_dir() {
  if [[ -z "${XDG_RUNTIME_DIR:-}" || ! -w "${XDG_RUNTIME_DIR:-/nonexistent}" ]]; then
    XDG_RUNTIME_DIR="/tmp/sonora-runtime-$(id -u)"
    export XDG_RUNTIME_DIR
    mkdir -p "$XDG_RUNTIME_DIR" && chmod 0700 "$XDG_RUNTIME_DIR"
  fi
  STATE="$XDG_RUNTIME_DIR/sonora-dev"
  # 0700: the engine refuses a control socket in a directory others can open.
  mkdir -p "$STATE" && chmod 0700 "$STATE"
}

write_pipewire_config() {
  local d="$HOME/.config/pipewire/pipewire.conf.d"
  mkdir -p "$d"
  # The pin the image ships (ADR-0003): quantum 128, 48 kHz. The engine
  # refuses a server at another rate, and the tunnel resamples to whatever
  # WSLg runs at.
  cp "$ROOT/audio-stack/pipewire-sonora.conf" "$d/99-sonora.conf"
  cat >"$d/90-sonora-wslg.conf" <<EOF
# Written by tools/dev-wsl.sh. WSL2 has no sound card: WSLg exposes the
# Windows default output and input as a PulseAudio server on
# $WSLG_PULSE. These two tunnels make them PipeWire nodes, so the
# engine's JACK ports can be linked to them.
context.modules = [
    { name = libpipewire-module-pulse-tunnel
      args = {
          tunnel.mode = sink
          pulse.server.address = "unix:$WSLG_PULSE"
          audio.rate = 48000
          audio.channels = 2
          audio.position = [ FL FR ]
          stream.props = {
              node.name = "$SPEAKERS"
              node.description = "Windows speakers (WSLg)"
          }
      }
    }
    { name = libpipewire-module-pulse-tunnel
      args = {
          tunnel.mode = source
          pulse.server.address = "unix:$WSLG_PULSE"
          audio.rate = 48000
          audio.channels = 2
          audio.position = [ FL FR ]
          stream.props = {
              node.name = "$MIC"
              node.description = "Windows microphone (WSLg)"
          }
      }
    }
]
EOF
}

pw_up() { pw-cli info 0 >/dev/null 2>&1; }

wait_for() {  # seconds, then a command
  local n=$(( $1 * 5 ))
  shift
  for _ in $(seq 1 "$n"); do "$@" && return 0; sleep 0.2; done
  return 1
}

node_ports() {  # node, -o|-i: its ports, monitor ports excluded
  pw-link "$2" 2>/dev/null | grep "^$1:" | grep -v ":monitor_" || true
}

do_audio() {
  check_host
  check_wslg
  runtime_dir
  write_pipewire_config
  local ours="$STATE/pipewire.pid"
  if command -v systemctl >/dev/null && systemctl --user is-active -q pipewire 2>/dev/null; then
    # systemd is enabled in this distro and runs PipeWire per user. Restart
    # it once, so the drop-ins written above are read.
    if [[ ! -f "$STATE/systemd-restarted" ]] || ! node_ports "$SPEAKERS" -i | grep -q .; then
      say "restarting the user's PipeWire (systemd) to load the WSLg bridge"
      systemctl --user restart pipewire wireplumber
      touch "$STATE/systemd-restarted"
    fi
  elif pw_up; then
    say "PipeWire is already running; reusing it"
  else
    say "starting PipeWire and WirePlumber (logs in $STATE)"
    if [[ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]]; then
      # WirePlumber needs a session bus; WSL without systemd has none.
      eval "$(dbus-launch --sh-syntax)"
      echo "export DBUS_SESSION_BUS_ADDRESS='$DBUS_SESSION_BUS_ADDRESS'" >"$STATE/dbus.env"
    fi
    setsid pipewire >"$STATE/pipewire.log" 2>&1 < /dev/null &
    echo $! >"$ours"
    setsid wireplumber >"$STATE/wireplumber.log" 2>&1 < /dev/null &
    echo $! >"$STATE/wireplumber.pid"
  fi
  wait_for 10 pw_up || die "PipeWire never answered; see $STATE/pipewire.log"
  if ! wait_for 10 bash -c "pw-link -i 2>/dev/null | grep -q '^$SPEAKERS:'"; then
    ls /usr/lib/*/pipewire-0.3/libpipewire-module-pulse-tunnel.so >/dev/null 2>&1 ||
      die "PipeWire's pulse-tunnel module is not installed (libpipewire-0.3-modules)"
    die "the $SPEAKERS node never appeared; see $STATE/pipewire.log"
  fi
  say "PipeWire up; speakers: $(node_ports "$SPEAKERS" -i | tr '\n' ' ')"
  if wait_for 5 bash -c "pw-link -o 2>/dev/null | grep -q '^$MIC:'"; then
    say "microphone: $(node_ports "$MIC" -o | tr '\n' ' ')"
  else
    say "no microphone node; Windows may deny WSLg the mic (Settings > Privacy > Microphone)"
  fi
  local clock
  clock="$(pw-metadata -n settings 0 2>/dev/null |
    sed -n "s/.*key:'clock\.\(rate\|quantum\)' value:'\([0-9]*\)'.*/\1=\2/p" | tr '\n' ' ' || true)"
  [[ -n "$clock" ]] && say "graph clock: $clock"
}

# --- patching --------------------------------------------------------------

# The engine connects only physical capture ports by itself, and the tunnels
# are not physical, so the patch is made here: the source's channels to
# sonora:in_1/in_2, sonora:out_1/out_2 (the monitor bus) to the speakers.
patch_engine() {  # source node
  local src="$1"
  wait_for 30 bash -c "pw-link -i 2>/dev/null | grep -q '^sonora:in_1$' &&
                       pw-link -o 2>/dev/null | grep -q '^sonora:out_1$'" || {
    echo "dev-wsl: the engine's ports never appeared; nothing patched" >&2
    return 1
  }
  mapfile -t outs < <(node_ports "$src" -o)
  mapfile -t spk < <(node_ports "$SPEAKERS" -i)
  if ((${#outs[@]})); then
    pw-link "${outs[0]}" sonora:in_1 2>/dev/null || true
    pw-link "${outs[${#outs[@]}>1 ? 1 : 0]}" sonora:in_2 2>/dev/null || true
  else
    echo "dev-wsl: no $src ports; the tracks record silence" >&2
  fi
  if ((${#spk[@]})); then
    pw-link sonora:out_1 "${spk[0]}" 2>/dev/null || true
    pw-link sonora:out_2 "${spk[${#spk[@]}>1 ? 1 : 0]}" 2>/dev/null || true
  fi
  echo "dev-wsl: patched $src -> sonora:in_1/2, sonora:out_1/2 -> $SPEAKERS"
}

engine_args() {
  ENGINE_EXTRA=()
  [[ "${SONORA_MONITOR:-0}" == 1 ]] || ENGINE_EXTRA+=(--no-input-monitoring)
}

# --- run -------------------------------------------------------------------

do_run() {
  do_audio
  [[ -x "$BUILD/sonora-engine" && -x "$BUILD/sonora-ui" ]] ||
    die "no build in $BUILD; run: tools/dev-wsl.sh build"
  engine_args
  # A fresh WSL Debian sets no locale, and Qt warns without a UTF-8 one.
  export LANG="${LANG:-C.UTF-8}"
  patch_engine "$MIC" >"$STATE/patch.log" 2>&1 &
  say "engine + Qt UI; session in $SESSION_DIR"
  SONORA_BUILD="$BUILD" SONORA_ENGINE_ARGS="${ENGINE_EXTRA[*]}" \
    "$ROOT/tools/run-sonora.sh" --backend jack --tracks 2 --dir "$SESSION_DIR" "$@"
}

# --- selftest --------------------------------------------------------------

levels() {  # wav files: peak and RMS in dBFS, per file
  python3 - "$@" <<'PY'
import math, sys, wave
for path in sys.argv[1:]:
    try:
        w = wave.open(path)
    except Exception as e:
        print(f"  {path}: unreadable ({e})"); continue
    width, n = w.getsampwidth(), w.getnframes() * w.getnchannels()
    raw = w.readframes(w.getnframes())
    full = float(1 << (8 * width - 1))
    peak = acc = 0.0
    for i in range(0, n * width, width):
        b = raw[i:i + width]
        v = int.from_bytes(b, "little", signed=width > 1)
        if width == 1: v -= 128
        x = abs(v) / full
        peak = max(peak, x); acc += x * x
    db = lambda x: f"{20 * math.log10(x):6.1f}" if x > 0 else "  -inf"
    rms = math.sqrt(acc / n) if n else 0.0
    print(f"  {path}: {w.getnframes() / w.getframerate():.1f} s, peak {db(peak)} dBFS, rms {db(rms)} dBFS")
PY
}

do_selftest() {
  local tone=0 seconds=4
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --tone) tone=1; shift ;;
      [0-9]*) seconds="$1"; shift ;;
      *) die "selftest: unknown argument $1" ;;
    esac
  done
  do_audio
  [[ -x "$BUILD/sonora-engine" && -x "$BUILD/sonora-ctl" ]] ||
    die "no build in $BUILD; run: tools/dev-wsl.sh build"
  engine_args
  local dir="$STATE/selftest-session" sock="$STATE/selftest.sock" src="$MIC"
  rm -rf "$dir" "$STATE/monitor.wav"
  PIDS=()
  trap 'for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null || true; done' EXIT

  if ((tone)); then
    # No microphone needed: 440 Hz left, 660 Hz right into a null sink,
    # whose monitor is what the engine records.
    src=sonora-tone
    python3 - "$STATE/tone.wav" "$((seconds + 6))" <<'PY'
import math, struct, sys, wave
path, secs = sys.argv[1], int(sys.argv[2])
with wave.open(path, "wb") as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(48000)
    w.writeframes(b"".join(struct.pack("<hh",
        int(8000 * math.sin(2 * math.pi * 440 * i / 48000)),
        int(8000 * math.sin(2 * math.pi * 660 * i / 48000))) for i in range(48000 * secs)))
PY
    # Lingers, or it would go with pw-cli; made once, reused by later runs.
    pw-link -o 2>/dev/null | grep -q "^$src:monitor_FL" ||
      pw-cli create-node adapter "{ factory.name=support.null-audio-sink node.name=$src
          media.class=Audio/Sink audio.position=[FL FR] priority.session=0
          object.linger=true }" >/dev/null
    wait_for 5 bash -c "pw-link -o 2>/dev/null | grep -q '^$src:monitor_FL'" ||
      die "the tone sink never appeared"
    pw-play --target "$src" "$STATE/tone.wav" >/dev/null 2>&1 &
    PIDS+=($!)
  fi

  pw-jack "$BUILD/sonora-engine" --backend jack --tracks 2 --dir "$dir" --headless \
    --no-memlock "${ENGINE_EXTRA[@]}" --control-socket "$sock" \
    >"$STATE/engine.out" 2>"$STATE/engine.err" &
  local engine=$!
  PIDS+=("$engine")
  if ((tone)); then
    wait_for 30 bash -c "pw-link -i 2>/dev/null | grep -q '^sonora:in_1$'" || {
      cat "$STATE/engine.err" >&2
      die "the engine's ports never appeared"
    }
    pw-link "$src:monitor_FL" sonora:in_1 && pw-link "$src:monitor_FR" sonora:in_2
    patch_engine /nonexistent-source >/dev/null 2>&1 || true
  else
    patch_engine "$src"
  fi
  wait_for 30 test -S "$sock" || die "the engine never opened $sock; see $STATE/engine.err"

  say "recording ${seconds}s from $src, then playing it back"
  local ctl=("$BUILD/sonora-ctl" --control-socket "$sock")
  "${ctl[@]}" take "$seconds" | tee "$STATE/take.out" || true
  # The take's own check plays a tenth of a second; this plays all of it,
  # with the speakers' monitor recorded to show what reached Windows.
  pw-record --target "$SPEAKERS" -P '{ stream.capture.sink = true }' --format s16 \
    "$STATE/monitor.wav" >/dev/null 2>&1 &
  local rec=$!
  PIDS+=("$rec")
  # Armed tracks are silent on playback while input monitoring is off.
  "${ctl[@]}" send track.disarm_all >/dev/null
  "${ctl[@]}" send transport.return_to_zero >/dev/null
  "${ctl[@]}" send transport.play
  sleep "$seconds"
  "${ctl[@]}" send transport.stop >/dev/null
  sleep 0.5
  kill -INT "$rec" 2>/dev/null || true
  wait "$rec" 2>/dev/null || true
  "${ctl[@]}" state | grep -i "xrun\|overrun\|transport" || true
  kill -TERM "$engine" 2>/dev/null || true
  wait "$engine" 2>/dev/null || true

  say "levels (the take, then what the speakers were sent):"
  levels "$dir/Audio Files/"*.wav "$STATE/monitor.wav"
  grep -h "^sonora:" "$STATE/engine.out" "$STATE/engine.err" | tail -5 || true
  grep -q "^RESULT checks_failed=0 " "$STATE/take.out" ||
    die "the take's checks failed (see above); with no mic, try: tools/dev-wsl.sh selftest --tone"
  say "selftest passed; the files are in $dir"
}

# --- main ------------------------------------------------------------------

cmd="${1:-all}"
[[ $# -gt 0 ]] && shift
case "$cmd" in
  deps) do_deps ;;
  build) do_build ;;
  audio) do_audio ;;
  run) [[ "${1:-}" == -- ]] && shift; do_run -- "$@" ;;
  selftest) do_selftest "$@" ;;
  all|--) [[ "${1:-}" == -- ]] && shift; do_deps; do_build; do_run -- "$@" ;;
  -h|--help) sed -n '2,35p' "$0" | sed 's/^# \{0,1\}//' ;;
  *) die "unknown step $cmd (see --help)" ;;
esac
