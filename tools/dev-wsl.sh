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
#   tools/dev-wsl.sh stop       stop the engine and UI (PipeWire stays)
#   tools/dev-wsl.sh status     what is running; exit 0 when the DAW is up
#   tools/dev-wsl.sh selftest [--tone] [SECONDS]
#                               record SECONDS (default 4) from the mic, or
#                               from a test tone, play it back to the
#                               speakers, and print the levels of the take
#                               and of what reached the speakers
#   tools/dev-wsl.sh measure [SECONDS] [--loop-out PORT --loop-in PORT]
#                               engine recording for SECONDS (default 600);
#                               prints the graph's quantum and rate every
#                               30 s, the engine's xruns, and the round-trip
#                               latency (jack_iodelay) when a loop exists:
#                               by default the speakers to the microphone
#                               (needs a working mic), or two JACK ports
#                               joined by a cable
#   tools/dev-wsl.sh user [NAME]  (as root) make NAME (default sonora, or the
#                               existing uid 1000) the distro's default user
#   tools/dev-wsl.sh --log FILE STEP ...
#                               the same, with the output also written to
#                               FILE (a Windows path such as C:/x/y.log works)
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
# Overridable only so the "WSLg does not answer" path can be tested.
WSLG_PULSE="${SONORA_WSLG_PULSE:-/mnt/wslg/PulseServer}"
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
  # A socket whose server is wedged is worse than none: the tunnel connects,
  # then nothing plays and the patching waits for ports that never come.
  if command -v pactl >/dev/null &&
     ! timeout 5 pactl -s "unix:$WSLG_PULSE" info >/dev/null 2>&1; then
    die "WSLg's PulseAudio server ($WSLG_PULSE) does not answer, so there is" \
        "no sound. From Windows run \`wsl --terminate ${WSL_DISTRO_NAME:-Debian}\`" \
        "(only this distro; never \`wsl --shutdown\`, it stops Docker Desktop)," \
        "then start Sonora again. If it still fails: \`wsl --update\`."
  fi
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
  dbus dbus-x11 python3 procps jack-example-tools
)

# Root needs no sudo, and a Debian distro installed with --no-launch has
# only root and may not have sudo at all.
as_root() {
  if ((EUID == 0)); then "$@"; else sudo "$@"; fi
}

do_deps() {
  check_host
  local missing=()
  for p in "${PACKAGES[@]}"; do
    dpkg-query -W -f='${Status}' "$p" 2>/dev/null | grep -q "install ok installed" ||
      missing+=("$p")
  done
  if ((${#missing[@]})); then
    say "installing ${#missing[@]} packages: ${missing[*]}"
    as_root apt-get update
    as_root env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${missing[@]}"
  else
    say "packages: all ${#PACKAGES[@]} installed"
  fi
  find_iodelay >/dev/null ||
    say "warning: no jack_iodelay after installing jack-example-tools; round-trip latency will be skipped (dpkg -S jack_iodelay)"
  # The same drop-in the image installs: pipewire-jack's libjack must win
  # over libjack-jackd2-0 (pulled in by the -dev package), or the engine
  # looks for a jackd that is not running.
  local conf=/etc/ld.so.conf.d/pipewire-jack.conf
  if [[ ! -f "$conf" ]]; then
    as_root install -D -m 0644 \
      /usr/share/doc/pipewire/examples/ld.so.conf.d/pipewire-jack-x86_64-linux-gnu.conf "$conf"
    as_root ldconfig
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

pw_up() { timeout 3 pw-cli info 0 >/dev/null 2>&1; }

wait_for() {  # seconds, then a command
  local n=$(( $1 * 5 ))
  shift
  for _ in $(seq 1 "$n"); do "$@" && return 0; sleep 0.2; done
  return 1
}

node_ports() {  # node, -o|-i: its ports, monitor ports excluded
  pw-link "$2" 2>/dev/null | grep "^$1:" | grep -v ":monitor_" || true
}

# WSL exports unix:path=/run/user/N/bus even when nothing listens there (no
# systemd), and PipeWire and WirePlumber then fail to reach a bus. A path
# address is dead when its socket is gone. The bus dbus-launch started for us
# is judged by its pid, whatever its address: after `wsl --terminate` its
# socket file can still sit in /tmp (the distro's disk) and an abstract one
# has no file at all, while the pid is gone or is now some other process.
bus_dead() {
  local a="${DBUS_SESSION_BUS_ADDRESS:-}" p="${DBUS_SESSION_BUS_PID:-}"
  [[ -z $a ]] && return 0
  if [[ -n $p ]]; then
    kill -0 "$p" 2>/dev/null || return 0
    [[ "$(cat "/proc/$p/comm" 2>/dev/null)" == dbus-daemon ]] || return 0
  fi
  [[ $a == unix:path=* ]] || return 1
  a="${a#unix:path=}"
  [[ ! -S ${a%%,*} ]]
}

have_bus() {
  local saved="$STATE/dbus.env"
  # A file written before the pid was saved cannot be checked: drop it.
  if [[ -f $saved ]] && ! grep -q DBUS_SESSION_BUS_PID "$saved"; then
    rm -f "$saved"
  fi
  if bus_dead && [[ -f $saved ]]; then
    unset DBUS_SESSION_BUS_PID
    # shellcheck disable=SC1090
    . "$saved"
  fi
  if bus_dead; then
    # WirePlumber needs a session bus; WSL without systemd has none.
    unset DBUS_SESSION_BUS_ADDRESS DBUS_SESSION_BUS_PID
    eval "$(dbus-launch --sh-syntax)"
    printf "export DBUS_SESSION_BUS_ADDRESS='%s' DBUS_SESSION_BUS_PID='%s'\n" \
      "$DBUS_SESSION_BUS_ADDRESS" "$DBUS_SESSION_BUS_PID" >"$saved"
  fi
}

start_wireplumber() {
  have_bus
  setsid wireplumber >>"$STATE/wireplumber.log" 2>&1 < /dev/null &
  echo $! >"$STATE/wireplumber.pid"
  sleep 0.5
}

ensure_wireplumber() {
  pgrep -x wireplumber >/dev/null && return 0
  say "WirePlumber is not running; starting it"
  start_wireplumber
}

# The pipewire socket must answer before WirePlumber starts, or it exits with
# "Failed to connect to PipeWire".
start_cold() {
  say "starting PipeWire and WirePlumber (logs in $STATE)"
  : >"$STATE/pipewire.log"  # the tunnel's overrun lines can reach hundreds of MB
  : >"$STATE/wireplumber.log"
  have_bus
  setsid pipewire >"$STATE/pipewire.log" 2>&1 < /dev/null &
  echo $! >"$STATE/pipewire.pid"
  wait_for 10 pw_up || die "PipeWire never answered; see $STATE/pipewire.log"
  start_wireplumber
}

# Only this distro's pipewire and wireplumber: the pids this script saved,
# then a pkill fallback.
kill_ours() {
  local f p
  for f in "$STATE/wireplumber.pid" "$STATE/pipewire.pid"; do
    if [[ -f $f ]]; then
      p="$(cat "$f")"
      kill -TERM "$p" 2>/dev/null || true
      rm -f "$f"
    fi
  done
  pkill -TERM -x wireplumber 2>/dev/null || true
  pkill -TERM -x pipewire 2>/dev/null || true
  sleep 1
  pkill -KILL -x wireplumber 2>/dev/null || true
  pkill -KILL -x pipewire 2>/dev/null || true
  sleep 0.3
}

speakers_up() { pw-link -i 2>/dev/null | grep -q "^$SPEAKERS:"; }

# WSLg's PulseServer restarts now and then; the tunnel does not reconnect.
# A server that hangs instead never says so: the sink tunnel's buffer only
# fills ("overrun write:N filled:N + size:S > max:M"), because nothing on the
# WSLg side reads it any more. A stalled tunnel logs that line every cycle, so
# the log ends in a run of them; one overrun the tunnel recovered from is a
# single line that may still be the last one, and must not restart PipeWire.
tunnel_dead() {
  local tail full='playback_stream_process.*overrun write:.* > max:' n
  tail="$(tail -c 300000 "$STATE/pipewire.log" 2>/dev/null)"
  grep -v overrun <<<"$tail" | tail -n 5 |
    grep -qi 'connection failure\|connection terminated' && return 0
  tail -n 1 <<<"$tail" | grep -q "$full" || return 1
  n="$(tail -n 20 <<<"$tail" | grep -c "$full")"
  ((n >= 3))
}

# A restart of PipeWire drops every client, an engine started by `run` in
# another terminal included.
engine_running() { pgrep -x sonora-engine >/dev/null; }

# With systemd=true in wsl.conf, a non-root user gets systemd's pipewire.socket,
# which owns $XDG_RUNTIME_DIR/pipewire-0 but cannot start pipewire.service: that
# needs a user D-Bus (dbus-user-session), which Debian's WSL image lacks. The
# first client then fails the socket unit and the bridge comes up only after a
# 10 s wait and a restart. Stopping the socket hands the path to our PipeWire.
release_systemd_socket() {
  command -v systemctl >/dev/null || return 0
  systemctl --user is-active -q pipewire.socket 2>/dev/null || return 0
  systemctl --user is-active -q pipewire 2>/dev/null && return 0
  say "stopping systemd's pipewire.socket (no user D-Bus to start PipeWire with)"
  systemctl --user stop pipewire.socket 2>/dev/null || true
}

log_tails() {
  local l
  for l in pipewire wireplumber; do
    echo "--- last lines of $STATE/$l.log (overrun lines removed):" >&2
    grep -v overrun "$STATE/$l.log" 2>/dev/null | tail -n 12 >&2 || true
  done
}

do_audio() {
  check_host
  check_wslg
  runtime_dir
  write_pipewire_config
  if command -v systemctl >/dev/null && systemctl --user is-active -q pipewire 2>/dev/null; then
    # systemd is enabled in this distro and runs PipeWire per user. Restart
    # it once, so the drop-ins written above are read.
    if [[ ${AUDIO_KEEP:-0} != 1 ]] &&
      { [[ ! -f "$STATE/systemd-restarted" ]] || ! node_ports "$SPEAKERS" -i | grep -q .; }; then
      say "restarting the user's PipeWire (systemd) to load the WSLg bridge"
      systemctl --user restart pipewire wireplumber
      touch "$STATE/systemd-restarted"
    fi
  else
    release_systemd_socket
    if pw_up; then
      say "PipeWire is already running; reusing it"
      ensure_wireplumber
    else
      start_cold
    fi
    if [[ ${AUDIO_KEEP:-0} == 1 ]]; then
      # selftest beside a running engine: diagnose only, never restart.
      speakers_up && tunnel_dead &&
        say "the WSLg tunnel looks dead, but the engine is running; not restarting PipeWire (tools/dev-wsl.sh stop, then retry)"
    elif speakers_up && tunnel_dead; then
      say "the WSLg tunnel lost its connection; restarting PipeWire"
      kill_ours
      start_cold
    fi
    if [[ ${AUDIO_KEEP:-0} != 1 ]] && ! wait_for 10 speakers_up; then
      ls /usr/lib/*/pipewire-0.3/libpipewire-module-pulse-tunnel.so >/dev/null 2>&1 ||
        die "PipeWire's pulse-tunnel module is not installed (libpipewire-0.3-modules)"
      say "the $SPEAKERS node did not appear; restarting PipeWire once"
      kill_ours
      start_cold
    fi
  fi
  wait_for 10 pw_up || die "PipeWire never answered; see $STATE/pipewire.log"
  if ! wait_for 10 speakers_up; then
    log_tails
    die "the $SPEAKERS node never appeared (even after a restart); logs above"
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
  mkdir -p "$(dirname "$SESSION_DIR")"  # the engine creates only the last path component
  say "engine + Qt UI; session in $SESSION_DIR"
  SONORA_BUILD="$BUILD" SONORA_ENGINE_ARGS="${ENGINE_EXTRA[*]}" \
    run_until_stopped "$ROOT/tools/run-sonora.sh" --backend jack --tracks 2 --dir "$SESSION_DIR" "$@"
}

# The UI ended by SIGTERM exits 143. That is a clean end only when `stop`
# sent it, which do_stop records in $STATE/stopped; any other SIGTERM stays a
# failure. A marker left by a `stop` with nothing running is removed first.
run_until_stopped() {  # command...
  local rc=0
  rm -f "$STATE/stopped"
  "$@" || rc=$?
  if ((rc == 143)) && [[ -f "$STATE/stopped" ]]; then
    say "ended by tools/dev-wsl.sh stop"
    rc=0
  fi
  rm -f "$STATE/stopped"
  return "$rc"
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
  if engine_running; then
    say "an engine is running; PipeWire is used as it is, never restarted"
    AUDIO_KEEP=1
  fi
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

# --- measure ---------------------------------------------------------------

# `pw-top -b` text on stdin (columns S ID QUANT RATE WAIT BUSY W/Q B/Q ERR
# FORMAT NAME): "RATE/QUANTUM" of the node that is running, or "?/?" when the
# graph is idle (every row then shows QUANT 0 RATE 0). A row for the engine's
# own client (NAME "sonora") wins; else the last row with both numbers > 0.
clock_of() {
  awk '$3 ~ /^[0-9]+$/ && $4 ~ /^[0-9]+$/ && $3 > 0 && $4 > 0 {
         if ($NF == "sonora") { er = $4; eq = $3 } else { r = $4; q = $3 } }
       END { if (eq) print er "/" eq; else print (q ? r "/" q : "?/?") }'
}

# `pw-metadata -n settings` on stdin -> "RATE/QUANTUM" as configured (a forced
# value wins over the default), "?/?" when it names neither.
clock_conf() {
  grep -o "key:'clock\.[a-z-]*' value:'[0-9]*'" | tr -d "'" | awk '
    { split($0, a, " "); k = substr(a[1], 5); v[k] = substr(a[2], 7) + 0 }
    END { r = v["clock.force-rate"]; if (!r) r = v["clock.rate"]
          q = v["clock.force-quantum"]; if (!q) q = v["clock.quantum"]
          print (r && q ? r "/" q : "?/?") }'
}

# Where jack_iodelay is: on PATH, else in the files of the packages that
# ship it (Debian moved it between jackd2 and jack-example-tools).
find_iodelay() {
  local f p
  command -v jack_iodelay && return 0
  for p in jack-example-tools jackd2 jackd2-firewire; do
    f="$(dpkg -L "$p" 2>/dev/null | grep -E '/jack_iodelay$' | head -n 1 || true)"
    if [[ -n $f && -x $f ]]; then echo "$f"; return 0; fi
  done
  for f in /usr/bin /usr/local/bin /usr/lib/jack /usr/lib/x86_64-linux-gnu/jack; do
    if [[ -x $f/jack_iodelay ]]; then echo "$f/jack_iodelay"; return 0; fi
  done
  return 1
}

# jack_iodelay output on stdin -> "<ms> ms (<frames> frames)" of the last
# "total roundtrip latency" line, or nothing when it never locked on. Always
# succeeds: under set -e -o pipefail a grep with no match would end the script
# before the report is written.
iodelay_last() {
  { grep -E "total roundtrip latency" || true; } | tail -n 1 |
    sed -E 's/^ *([0-9.]+) frames +([0-9.]+) ms.*/\2 ms (\1 frames)/'
}

# stdin: one "RATE/QUANTUM" per line. Succeeds when there is at least one
# sample that saw the graph running, and all of those equal: a quantum that
# moves mid-session changes our block size. "?/?" is an idle sample (the take
# had not started or had ended): counted apart, not a verdict.
clock_stable() {
  local lines
  lines="$(sort -u | grep -v '?' | grep . || true)"
  [[ -n $lines && $(wc -l <<<"$lines") -eq 1 ]]
}

do_measure() {
  local seconds=600 lout="" lin=""
  while [[ $# -gt 0 ]]; do
    case "$1" in
      [0-9]*) seconds="$1"; shift ;;
      --loop-out) lout="${2:?--loop-out needs a port}"; shift 2 ;;
      --loop-in) lin="${2:?--loop-in needs a port}"; shift 2 ;;
      *) die "measure: unknown argument $1" ;;
    esac
  done
  if engine_running; then
    say "an engine is running; PipeWire is used as it is, never restarted"
    AUDIO_KEEP=1
  fi
  do_audio
  [[ -x "$BUILD/sonora-engine" && -x "$BUILD/sonora-ctl" ]] ||
    die "no build in $BUILD; run: tools/dev-wsl.sh build"
  engine_args
  local dir="$STATE/measure-session" sock="$STATE/measure.sock" out="$STATE/measure.txt"
  rm -rf "$dir"
  : >"$out"
  PIDS=()
  trap 'for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null || true; done' EXIT
  pw-jack "$BUILD/sonora-engine" --backend jack --tracks 2 --dir "$dir" --headless \
    --no-memlock "${ENGINE_EXTRA[@]}" --control-socket "$sock" \
    >"$STATE/engine.out" 2>"$STATE/engine.err" &
  local engine=$!
  PIDS+=("$engine")
  patch_engine "$MIC" || true
  wait_for 30 test -S "$sock" || die "the engine never opened $sock; see $STATE/engine.err"
  local ctl=("$BUILD/sonora-ctl" --control-socket "$sock")

  # Round trip first: it needs the engine's graph only as company, and a
  # missing loop must not cost ten minutes before it is reported.
  local lat="not measured"
  local iodelay
  if ! iodelay="$(find_iodelay)"; then
    lat="not measured: jack_iodelay not found (run: tools/dev-wsl.sh deps; dpkg -S jack_iodelay)"
  else
    if [[ -z "$lout" || -z "$lin" ]]; then
      mapfile -t spk < <(node_ports "$SPEAKERS" -i)
      mapfile -t mic < <(node_ports "$MIC" -o)
      lout="${spk[0]:-}"; lin="${mic[0]:-}"
    fi
    if [[ -z "$lout" || -z "$lin" ]]; then
      lat="not measured: no loop ports (pass --loop-out and --loop-in)"
    else
      pw-jack "$iodelay" >"$STATE/iodelay.out" 2>&1 &
      local iod=$!
      PIDS+=("$iod")
      if wait_for 10 bash -c "pw-link -i 2>/dev/null | grep -q '^jack_delay:in$'"; then
        pw-link jack_delay:out "$lout" 2>/dev/null || true
        pw-link "$lin" jack_delay:in 2>/dev/null || true
        sleep 12
        lat="$(iodelay_last <"$STATE/iodelay.out" || true)"
        [[ -n "$lat" ]] ||
          lat="not measured: no signal came back through $lout -> $lin (silent microphone or no cable)"
      else
        lat="not measured: jack_iodelay did not start"
      fi
      kill "$iod" 2>/dev/null || true
      wait "$iod" 2>/dev/null || true
    fi
  fi

  say "measuring ${seconds}s; the graph clock is sampled every 30 s while the take rolls"
  "${ctl[@]}" take "$seconds" >"$STATE/take.out" 2>&1 &
  local take=$!
  PIDS+=("$take")
  # The take arms and starts recording first; an idle graph before that is
  # not a quantum change. It records for $seconds and then plays the take
  # back, so the window ends when the take does.
  sleep 5
  local t=0 clocks=() top="" rolling="" conf one i
  conf="$(pw-metadata -n settings 0 2>/dev/null | clock_conf || true)"
  while kill -0 "$take" 2>/dev/null && ((t < 2 * seconds + 60)); do
    top="$(pw-top -b -n 2 2>/dev/null || true)"
    one="$(clock_of <<<"$top")"
    clocks+=("$one")
    [[ $one != '?/?' ]] && rolling="$top"
    i=0
    while ((i < 30)) && kill -0 "$take" 2>/dev/null; do sleep 1; i=$((i + 1)); done
    t=$((t + 30))
  done
  wait "$take" 2>/dev/null || true
  # The socket may serve one client at a time: ask again once the take has
  # let go, and keep what it said when it still gives no state.
  local state="" raw=""
  for _ in 1 2 3; do
    raw="$("${ctl[@]}" state 2>&1 || true)"
    state="$(grep -i "xruns=" <<<"$raw" || true)"
    [[ -n $state ]] && break
    sleep 1
  done
  [[ -n $state ]] || state="$(grep -i "xrun\|overrun" "$STATE/take.out" || true)"
  kill -TERM "$engine" 2>/dev/null || true
  wait "$engine" 2>/dev/null || true

  local distinct idle
  distinct="$(printf '%s\n' "${clocks[@]}" | grep -v '?' | sort -u | paste -sd' ' - || true)"
  idle="$(printf '%s\n' "${clocks[@]}" | grep -c '?' || true)"
  {
    echo "dev-wsl measure: $(date -u +%FT%TZ), ${seconds}s, ${#clocks[@]} clock samples ($idle idle)"
    echo "clock configured (pw-metadata) rate/quantum: $conf"
    echo "clock running (pw-top) rate/quantum seen: ${distinct:-none}"
    if printf '%s\n' "${clocks[@]}" | clock_stable; then echo "quantum: stable"; else echo "quantum: NOT stable or never seen rolling"; fi
    if [[ -n $state ]]; then
      echo "engine: $state"
    else
      echo "engine: no state; sonora-ctl state said: ${raw:-nothing}"
      echo "engine stderr (tail): $(tail -n 3 "$STATE/engine.err" 2>/dev/null | paste -sd'|' -)"
    fi
    echo "take: $(grep '^RESULT' "$STATE/take.out" || echo "no RESULT line; $(tail -n 2 "$STATE/take.out" | paste -sd'|' -)")"
    echo "pw-top while rolling (ERR column):"
    printf '%s\n' "${rolling:-no sample saw the graph running}" | tail -n 12
    echo "round-trip latency: $lat"
  } | tee "$out"
  say "also written to $out; the take checks are in $STATE/take.out"
}

# --- user ------------------------------------------------------------------

# wsl.conf on stdin -> stdout with default=NAME in its [user] section. The
# first [user] section gets the line, every other default= under a [user]
# header goes, and with no [user] section one is appended. awk ends every line
# with a newline, so nothing is glued onto a last line that had none. A file
# saved from Notepad ends its lines in CR, and a header may carry a comment
# ("[user] # WSL"): both are dropped from the copy the header is matched on.
wsl_conf_set_user() {  # name
  awk -v name="$1" '
    /^[ \t]*\[/ {
      sect = $0; sub(/[#;].*/, "", sect); gsub(/[ \t\r\[\]]/, "", sect); sect = tolower(sect)
      print
      if (sect == "user" && !done) { print "default=" name; done = 1 }
      next
    }
    sect == "user" && /^[ \t]*default[ \t]*=/ { next }
    { print }
    END { if (!done) { if (NR) print ""; print "[user]"; print "default=" name } }
  '
}

# The NOPASSWD line for NAME in DIR (/etc/sudoers.d). Before #284 it was
# written to 90-NAME; that copy goes, but only when it holds exactly our line.
sudoers_set() {  # dir name
  local dir="$1" name="$2" line="$2 ALL=(ALL) NOPASSWD:ALL"
  local sudoers="$dir/90-sonora-$name" legacy="$dir/90-$name"
  if [[ "$(cat "$sudoers" 2>/dev/null)" != "$line" ]]; then
    printf '%s\n' "$line" >"$sudoers.tmp"
    chmod 0440 "$sudoers.tmp"
    visudo -cqf "$sudoers.tmp" || { rm -f "$sudoers.tmp"; die "visudo rejected $sudoers"; }
    mv -f "$sudoers.tmp" "$sudoers"
  fi
  if [[ -f $legacy && "$(cat "$legacy")" == "$line" ]]; then
    rm -f "$legacy"
    say "removed $legacy (the same line is in $sudoers)"
  fi
}

# The operator's own wsl.conf is copied to CONF.sonora-bak before this script
# first changes it, and never overwritten after, so it can always be put back.
wsl_conf_write() {  # conf name
  local conf="$1" name="$2" old="" new
  [[ -f $conf ]] && old="$(cat "$conf")"
  new="$(printf '%s' "$old" | wsl_conf_set_user "$name")"
  if [[ "$old" == "$new" ]]; then
    say "$conf: default user already '$name'"
    return 0
  fi
  if [[ -f $conf && ! -e $conf.sonora-bak ]]; then
    cp -p "$conf" "$conf.sonora-bak"
    say "$conf: the previous version is kept in $conf.sonora-bak"
  fi
  # Written beside it, then renamed: a half-written wsl.conf would be read
  # at the next start of the distro.
  printf '%s\n' "$new" >"$conf.sonora-tmp"
  chmod 0644 "$conf.sonora-tmp"
  mv -f "$conf.sonora-tmp" "$conf"
  say "$conf: default user is now '$name' (read when the distro next starts)"
}

do_user() {
  local name="${1:-sonora}" conf="${SONORA_WSL_CONF:-/etc/wsl.conf}" existing
  ((EUID == 0)) || die "user: run as root (tools/dev-wsl.ps1 user does that)"
  [[ $name =~ ^[a-z][a-z0-9_-]*$ ]] || die "'$name' is not a valid Linux user name"
  # WSL's own first-launch user is uid 1000: reuse it rather than add a second.
  existing="$(getent passwd 1000 | cut -d: -f1 || true)"
  if [[ -n $existing ]]; then
    if [[ $existing != "$name" ]]; then
      # On stdout: dev-wsl.ps1 prints this step's stdout, and Windows
      # PowerShell 5.1 turns a native command's stderr into error records.
      say "WARNING: uid 1000 is '$existing', not '$name': '$existing' becomes the default user"
      say "WARNING: and gets sudo without a password (/etc/sudoers.d/90-sonora-$existing)"
    fi
    name="$existing"
  elif getent passwd "$name" >/dev/null; then
    say "user '$name' exists"
  else
    useradd -m -s /bin/bash "$name"
    say "user '$name' created"
  fi
  if ! command -v sudo >/dev/null; then
    say "installing sudo"
    apt-get update
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends sudo
  fi
  usermod -aG sudo,audio,video "$name"
  sudoers_set /etc/sudoers.d "$name"
  wsl_conf_write "$conf" "$name"
  echo "DEFAULT_USER=$name"
}

# --- stop, status ----------------------------------------------------------

do_stop() {
  # Engine and UI only: PipeWire stays up.
  runtime_dir
  : >"$STATE/stopped"  # before the signal, so `run` sees it when the UI ends
  pkill -TERM -x sonora-ui 2>/dev/null || true
  pkill -TERM -x sonora-engine 2>/dev/null || true
  for _ in $(seq 50); do  # up to 5 s for the engine to write its session
    pgrep -x sonora-engine >/dev/null || pgrep -x sonora-ui >/dev/null || break
    sleep 0.1
  done
  pkill -KILL -x sonora-engine 2>/dev/null || true
  pkill -KILL -x sonora-ui 2>/dev/null || true
  sleep 0.2
  if pgrep -x sonora-engine >/dev/null || pgrep -x sonora-ui >/dev/null; then
    die "could not stop the engine or UI"
  fi
  say "stopped"
}

do_status() {
  local up=0 n
  for n in sonora-engine sonora-ui pipewire; do
    if pgrep -x "$n" >/dev/null; then
      say "$n: running"
      [[ $n == pipewire ]] || up=1
    else
      say "$n: not running"
    fi
  done
  if [[ -S $WSLG_PULSE ]]; then say "WSLg audio: present"; else say "WSLg audio: MISSING"; fi
  if [[ -x "$BUILD/sonora-engine" ]]; then say "build: present"; else say "build: missing"; fi
  return $((1 - up))
}

# --- main ------------------------------------------------------------------

# Windows paths (C:/x or C:\x) reach the log through /mnt/<drive>.
linux_path() {
  if [[ $1 =~ ^[A-Za-z]:[/\\] ]]; then
    wslpath -u "$1" 2>/dev/null && return
    local d="${1:0:1}" r="${1:2}"
    echo "/mnt/${d,,}${r//\\//}"
  else
    echo "$1"
  fi
}

if [[ ${1:-} == --log ]]; then
  # The tee runs here, not in PowerShell: Windows PowerShell 5.1 turns a
  # native command's stderr into error records and writes Tee-Object files in
  # UTF-16. This file gets the script's own UTF-8 bytes, stderr included.
  [[ $# -ge 2 ]] || die "--log needs a file"
  log="$(linux_path "$2")"
  shift 2
  mkdir -p "$(dirname "$log")"
  rc=0
  bash "${BASH_SOURCE[0]}" "$@" 2>&1 | tee "$log" || rc=$?
  exit "$rc"
fi

cmd="${1:-all}"
[[ $# -gt 0 ]] && shift
case "$cmd" in
  deps) do_deps ;;
  build) do_build ;;
  audio) do_audio ;;
  run) [[ "${1:-}" == -- ]] && shift; do_run -- "$@" ;;
  stop) do_stop ;;
  status) do_status ;;
  selftest) do_selftest "$@" ;;
  measure) do_measure "$@" ;;
  user) do_user "$@" ;;
  all|--) [[ "${1:-}" == -- ]] && shift; do_deps; do_build; do_run -- "$@" ;;
  -h|--help) sed -n '2,42p' "$0" | sed 's/^# \{0,1\}//' ;;
  *) die "unknown step $cmd (see --help)" ;;
esac
