#!/usr/bin/env bash
# The parts of tools/dev-wsl.sh that need neither WSL nor root: the wsl.conf
# edit, wsl.conf backup and sudoers files of the `user` step, the tunnel and
# restart checks of `audio`, the `stop` marker of `run`, the session-bus
# liveness check, the Windows log path and the --log wrapper. Runs on any Linux (CI's "Shell scripts" job).
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
S="$ROOT/tools/dev-wsl.sh"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
fail=0
ok() { echo "PASS $*"; }
bad() { echo "FAIL $*"; fail=1; }

# The functions without the dispatch at the bottom.
sed '/^# --- main ---/,$d' "$S" >"$TMP/fns.sh"
eval "$(sed -n '/^linux_path() {/,/^}/p' "$S")"
# shellcheck disable=SC1091
. "$TMP/fns.sh"
set +e
# Read by bus_dead and have_bus, which this file only calls.
export DBUS_SESSION_BUS_ADDRESS DBUS_SESSION_BUS_PID

conf() {  # label, wsl.conf content (printf format), expected output
  local got
  # shellcheck disable=SC2059
  got="$(printf "$2" | wsl_conf_set_user sonora)"
  if [[ $got == "$3" ]]; then ok "wsl.conf $1"; else bad "wsl.conf $1: $(printf '%q' "$got")"; fi
}
conf "empty" '' $'[user]\ndefault=sonora'
conf "last line without newline" '[boot]\nsystemd=true' $'[boot]\nsystemd=true\n\n[user]\ndefault=sonora'
conf "other default replaced" '[user]\ndefault=bob\n[boot]\nsystemd=true\n' $'[user]\ndefault=sonora\n[boot]\nsystemd=true'
conf "two [user] sections" '[user]\ndefault=a\n[network]\nhostname=x\n[user]\ndefault=b\n' \
  $'[user]\ndefault=sonora\n[network]\nhostname=x\n[user]'
conf "default= of another section kept" '[network]\ndefault=keep\n' $'[network]\ndefault=keep\n\n[user]\ndefault=sonora'
conf "CRLF header" '[user]\r\ndefault=bob\r\n' $'[user]\r\ndefault=sonora'
conf "header with a comment" '[user] # set by hand\ndefault=bob\n' $'[user] # set by hand\ndefault=sonora'
conf "commented-out header" '# [user]\n[boot]\nsystemd=true\n' $'# [user]\n[boot]\nsystemd=true\n\n[user]\ndefault=sonora'
once="$(printf '[boot]\nsystemd=true' | wsl_conf_set_user sonora)"
twice="$(printf '%s\n' "$once" | wsl_conf_set_user sonora)"
if [[ $once == "$twice" ]]; then ok "wsl.conf edit is idempotent"; else bad "wsl.conf second edit differs"; fi

# The operator's wsl.conf is kept once, before the first change.
printf '[boot]\nsystemd=true\n' >"$TMP/wsl.conf"
wsl_conf_write "$TMP/wsl.conf" a >/dev/null
if [[ $(cat "$TMP/wsl.conf.sonora-bak" 2>/dev/null) == $'[boot]\nsystemd=true' ]]; then ok "wsl.conf backed up"; else bad "no wsl.conf backup"; fi
wsl_conf_write "$TMP/wsl.conf" b >/dev/null
if grep -q default=b "$TMP/wsl.conf" && ! grep -q default "$TMP/wsl.conf.sonora-bak"; then
  ok "backup not overwritten by a later change"
else
  bad "backup overwritten or conf unchanged"
fi
wsl_conf_write "$TMP/none.conf" a >/dev/null
if [[ -f $TMP/none.conf && ! -e $TMP/none.conf.sonora-bak ]]; then ok "no wsl.conf: none backed up"; else bad "missing wsl.conf case"; fi

# sudoers: our file written, the pre-#284 duplicate removed only when it is ours.
# shellcheck disable=SC2317,SC2329  # called by sudoers_set
visudo() { :; }
mkdir -p "$TMP/sudoers.d"
echo 'a ALL=(ALL) NOPASSWD:ALL' >"$TMP/sudoers.d/90-a"
echo 'b ALL=(ALL) ALL' >"$TMP/sudoers.d/90-b"
sudoers_set "$TMP/sudoers.d" a >/dev/null
sudoers_set "$TMP/sudoers.d" b >/dev/null
if [[ $(cat "$TMP/sudoers.d/90-sonora-a") == 'a ALL=(ALL) NOPASSWD:ALL' ]]; then ok "sudoers written"; else bad "sudoers content"; fi
if [[ ! -e $TMP/sudoers.d/90-a ]]; then ok "old duplicate 90-NAME removed"; else bad "old 90-NAME kept"; fi
if [[ -e $TMP/sudoers.d/90-b ]]; then ok "a 90-NAME we did not write is kept"; else bad "foreign 90-NAME removed"; fi
unset -f visudo

unset DBUS_SESSION_BUS_ADDRESS DBUS_SESSION_BUS_PID
if bus_dead; then ok "no address: dead"; else bad "no address"; fi
DBUS_SESSION_BUS_ADDRESS=unix:path=$TMP/none
if bus_dead; then ok "path without socket: dead"; else bad "path without socket"; fi
DBUS_SESSION_BUS_ADDRESS=unix:abstract=/tmp/x,guid=1
if bus_dead; then bad "foreign abstract bus"; else ok "abstract bus with no pid: trusted"; fi
# A socket file left behind by a daemon that is gone: what /tmp holds after
# `wsl --terminate`.
python3 -c 'import socket,sys; socket.socket(socket.AF_UNIX).bind(sys.argv[1])' "$TMP/bus"
sleep 0 & gone=$!
wait "$gone"
DBUS_SESSION_BUS_ADDRESS=unix:path=$TMP/bus,guid=1 DBUS_SESSION_BUS_PID=$gone
if bus_dead; then ok "stale socket, daemon gone: dead"; else bad "stale socket, daemon gone"; fi
DBUS_SESSION_BUS_PID=$$
if bus_dead; then ok "pid reused by a non-dbus process: dead"; else bad "pid reused"; fi
DBUS_SESSION_BUS_ADDRESS=unix:abstract=/tmp/x,guid=1 DBUS_SESSION_BUS_PID=$gone
if bus_dead; then ok "abstract, daemon gone: dead"; else bad "abstract, daemon gone"; fi
unset DBUS_SESSION_BUS_ADDRESS DBUS_SESSION_BUS_PID

if command -v dbus-launch >/dev/null; then
  STATE="$TMP/state"
  mkdir -p "$STATE"
  have_bus
  first=$DBUS_SESSION_BUS_PID
  if ! bus_dead && grep -q DBUS_SESSION_BUS_PID "$STATE/dbus.env"; then ok "bus launched, pid saved"; else bad "bus launch"; fi
  unset DBUS_SESSION_BUS_ADDRESS DBUS_SESSION_BUS_PID
  have_bus
  if [[ $DBUS_SESSION_BUS_PID == "$first" ]]; then ok "saved bus reused"; else bad "saved bus not reused"; fi
  kill "$first"
  sleep 0.3
  unset DBUS_SESSION_BUS_ADDRESS DBUS_SESSION_BUS_PID
  have_bus
  if [[ $DBUS_SESSION_BUS_PID != "$first" ]] && ! bus_dead; then ok "stale dbus.env replaced"; else bad "stale dbus.env kept"; fi
  kill "$DBUS_SESSION_BUS_PID"
  echo "export DBUS_SESSION_BUS_ADDRESS='unix:abstract=/tmp/old'" >"$STATE/dbus.env"
  unset DBUS_SESSION_BUS_ADDRESS DBUS_SESSION_BUS_PID
  have_bus
  if grep -q DBUS_SESSION_BUS_PID "$STATE/dbus.env"; then ok "dbus.env without a pid replaced"; else bad "old dbus.env kept"; fi
  kill "$DBUS_SESSION_BUS_PID"
else
  echo "SKIP have_bus (no dbus-launch)"
fi

# The tunnel checks read $STATE/pipewire.log; lines as PipeWire 1.4 writes them.
STATE="$TMP/tunnel"
mkdir -p "$STATE"
ovr='[W][23:56:16.115996] mod.pulse-tunnel | [module-pulse-tun:  373 playback_stream_process()] 0x59d5: overrun write:38440120 filled:38440120 + size:512 > max:4194304'
printf '%s\n' '[W][10:11:34.474042] mod.pulse-tunnel | [module-pulse-tun:  719 stream_underflow_cb()] underflow (0 suppressed)' >"$STATE/pipewire.log"
if tunnel_dead; then bad "tunnel with an underflow only: dead"; else ok "tunnel with an underflow only: alive"; fi
printf '%s\n' "$ovr" >>"$STATE/pipewire.log"
if tunnel_dead; then bad "one recovered overrun, last line: dead"; else ok "one recovered overrun, last line: alive"; fi
printf '%s\n' "$ovr" >>"$STATE/pipewire.log"
if tunnel_dead; then bad "two overruns: dead"; else ok "two overruns: alive"; fi
printf '%s\n' "$ovr" >>"$STATE/pipewire.log"
if tunnel_dead; then ok "sink buffer full, nothing reads it: dead"; else bad "stalled sink tunnel not seen"; fi
# Three overruns long ago, then the tunnel ran on.
{ printf '%s\n' "$ovr" "$ovr" "$ovr"; for _ in $(seq 25); do echo '[I][x] pw.node | running'; done; echo "$ovr"; } >"$STATE/pipewire.log"
if tunnel_dead; then bad "old overruns plus one new: dead"; else ok "old overruns plus one new: alive"; fi
printf '%s\n' '[E][x] mod.pulse-tunnel | connection failure' "$ovr" >"$STATE/pipewire.log"
if tunnel_dead; then ok "connection failure under overrun lines: dead"; else bad "connection failure missed"; fi

# do_audio with every PipeWire call faked: a dead tunnel restarts PipeWire,
# except under AUDIO_KEEP (selftest beside a running engine).
audio_case() {  # label, AUDIO_KEEP, expect restart (1/0)
  local got
  : >"$TMP/audio.log"
  # shellcheck disable=SC2317,SC2329  # the fakes are called by do_audio
  (
    check_host() { :; }; check_wslg() { :; }; runtime_dir() { :; }
    write_pipewire_config() { :; }; ensure_wireplumber() { :; }
    systemctl() { return 1; }; pw_up() { :; }; speakers_up() { :; }; tunnel_dead() { :; }
    kill_ours() { echo kill >>"$TMP/audio.log"; }; start_cold() { echo start >>"$TMP/audio.log"; }
    node_ports() { :; }; pw-link() { return 1; }; pw-metadata() { :; }; sleep() { :; }
    AUDIO_KEEP=$2 do_audio
  ) >/dev/null 2>&1
  if grep -q kill "$TMP/audio.log"; then got=1; else got=0; fi
  if [[ $got == "$3" ]]; then ok "do_audio: $1"; else bad "do_audio: $1 (restart=$got)"; fi
}
audio_case "dead tunnel restarts PipeWire" 0 1
audio_case "dead tunnel beside a running engine: no restart" 1 0

# `run` ends 0 on a SIGTERM only when `stop` left its marker.
STATE="$TMP/run"
mkdir -p "$STATE"
run_until_stopped bash -c 'kill -TERM $$' >/dev/null
rc=$?
if [[ $rc == 143 ]]; then ok "SIGTERM without stop: 143"; else bad "SIGTERM without stop: $rc"; fi
# shellcheck disable=SC2016  # expanded by the inner bash
run_until_stopped bash -c ': >"$1/stopped"; kill -TERM $$' _ "$STATE" >/dev/null
rc=$?
if [[ $rc == 0 && ! -e $STATE/stopped ]]; then ok "SIGTERM from stop: 0, marker gone"; else bad "SIGTERM from stop: $rc"; fi
: >"$STATE/stopped"  # a stop with nothing running
run_until_stopped bash -c 'kill -TERM $$' >/dev/null
rc=$?
if [[ $rc == 143 ]]; then ok "stale marker ignored"; else bad "stale marker taken: $rc"; fi
# shellcheck disable=SC2016  # expanded by the inner bash
run_until_stopped bash -c ': >"$1/stopped"; exit 3' _ "$STATE" >/dev/null
rc=$?
if [[ $rc == 3 ]]; then ok "other exit codes kept"; else bad "exit 3 became $rc"; fi
# do_stop writes the marker before it signals anything.
rm -f "$STATE/stopped"
# shellcheck disable=SC2317,SC2329  # the fakes are called by do_stop
(
  runtime_dir() { :; }; pkill() { [[ -e $STATE/stopped ]] || echo early >>"$TMP/stop.log"; return 1; }
  pgrep() { return 1; }; sleep() { :; }
  do_stop
) >/dev/null 2>&1
if [[ -e $STATE/stopped && ! -e $TMP/stop.log ]]; then ok "stop marks before signalling"; else bad "stop marker missing or late"; fi

# A fake systemctl: unit states from $SC_ACTIVE, calls logged to $SC_LOG.
SC_LOG="$TMP/systemctl.log"
# shellcheck disable=SC2317,SC2329  # called by release_systemd_socket
systemctl() {
  echo "$*" >>"$SC_LOG"
  if [[ $2 == is-active ]]; then [[ " $SC_ACTIVE " == *" ${*: -1} "* ]]; fi
}
socket_case() {  # label, active units, expect stop (1/0)
  SC_ACTIVE="$2"
  : >"$SC_LOG"
  release_systemd_socket >/dev/null
  if grep -q "stop pipewire.socket" "$SC_LOG"; then got=1; else got=0; fi
  if [[ $got == "$3" ]]; then ok "systemd socket: $1"; else bad "systemd socket: $1 (stop=$got)"; fi
}
socket_case "socket without a service is stopped" "pipewire.socket" 1
socket_case "a running service is left alone" "pipewire.socket pipewire" 0
socket_case "no socket, nothing to do" "" 0
unset -f systemctl

# shellcheck disable=SC2317,SC2329  # called by linux_path
wslpath() { return 1; }  # outside WSL: the fallback is what is tested
if [[ $(linux_path 'C:\Users\A B\x.log') == '/mnt/c/Users/A B/x.log' ]]; then ok "C:\\ path"; else bad "C:\\ path: $(linux_path 'C:\Users\A B\x.log')"; fi
if [[ $(linux_path 'D:/y/z.log') == /mnt/d/y/z.log ]]; then ok "D:/ path"; else bad "D:/ path"; fi
if [[ $(linux_path /tmp/a) == /tmp/a ]]; then ok "linux path unchanged"; else bad "linux path"; fi

bash "$S" --log "$TMP/l/out.log" no-such-step >/dev/null 2>&1
rc=$?
if [[ $rc == 1 ]]; then ok "--log keeps the exit code"; else bad "--log exit $rc"; fi
if grep -q "unknown step no-such-step" "$TMP/l/out.log"; then ok "--log has stderr"; else bad "--log content"; fi
if LC_ALL=C grep -q -P '\x00' "$TMP/l/out.log"; then bad "--log is not UTF-8 (NUL bytes)"; else ok "--log has no UTF-16"; fi

# measure: parsers of the clock and of jack_iodelay.
top=$'S   ID  QUANT   RATE    WAIT    BUSY   W/Q   B/Q  ERR FORMAT  NAME\nS   30      0      0  0.0ns  0.0ns  ---  ---    0        Dummy-Driver\nR   45    128  48000 10.0us  20.0us 0.00 0.01    3 S16LE 2 48000 sonora\n'
if [[ $(clock_of <<<"$top") == 48000/128 ]]; then ok "clock_of"; else bad "clock_of: $(clock_of <<<"$top")"; fi
if [[ $(echo "nothing" | clock_of) == '?/?' ]]; then ok "clock_of unknown"; else bad "clock_of unknown"; fi
iod=$'  128.000 frames      2.667 ms total roundtrip latency\n  250.000 frames      5.208 ms total roundtrip latency\n'
if [[ $(iodelay_last <<<"$iod") == "5.208 ms (250.000 frames)" ]]; then ok "iodelay_last"; else bad "iodelay_last: $(iodelay_last <<<"$iod")"; fi
if [[ -z $(echo "no signal" | iodelay_last) ]]; then ok "iodelay_last empty"; else bad "iodelay_last empty"; fi
if printf '48000/128\n48000/128\n' | clock_stable; then ok "stable clock"; else bad "stable clock"; fi
if printf '48000/128\n48000/256\n' | clock_stable; then bad "moving quantum called stable"; else ok "moving quantum"; fi
if printf '?/?\n' | clock_stable; then bad "unknown called stable"; else ok "unknown clock"; fi
if : | clock_stable; then bad "no samples called stable"; else ok "no samples"; fi
if printf '?/?\n48000/128\n48000/128\n' | clock_stable; then ok "idle sample ignored"; else bad "idle sample ignored"; fi
if printf '?/?\n48000/128\n48000/64\n' | clock_stable; then bad "idle hid a moving quantum"; else ok "idle plus moving"; fi

# The idle graph exactly as pw-top printed it in WSL (every node at 0).
idle=$'S   ID  QUANT   RATE    WAIT    BUSY   W/Q   B/Q  ERR FORMAT           NAME \nC   33      0      0    ---     ---   ---   ---     0                  Dummy-Driver\nC   39      0      0    ---     ---   ---   ---     0                  wslg-speakers\nI   33      0      0   0.0us   0.0us  ???   ???     0                  Dummy-Driver\nS   56      0      0    ---     ---   ---   ---     0                  sonora-tone\n'
if [[ $(clock_of <<<"$idle") == '?/?' ]]; then ok "clock_of idle graph"; else bad "clock_of idle: $(clock_of <<<"$idle")"; fi
run=$'S   ID  QUANT   RATE    WAIT    BUSY   W/Q   B/Q  ERR FORMAT           NAME \nR   39    128  48000  20.0us  30.0us 0.02  0.03    0                  wslg-speakers\nR   57    128  48000  10.0us  20.0us 0.01  0.02    0   F32P 2 48000   sonora\nS   56      0      0    ---     ---   ---   ---     0                  sonora-tone\n'
if [[ $(clock_of <<<"$run") == 48000/128 ]]; then ok "clock_of running graph"; else bad "clock_of running: $(clock_of <<<"$run")"; fi
meta=$'Found "settings" metadata 32\nupdate: id:0 key:\'log.level\' value:\'2\' type:\'\'\nupdate: id:0 key:\'clock.rate\' value:\'48000\' type:\'\'\nupdate: id:0 key:\'clock.quantum\' value:\'128\' type:\'\'\nupdate: id:0 key:\'clock.force-quantum\' value:\'0\' type:\'\'\n'
if [[ $(clock_conf <<<"$meta") == 48000/128 ]]; then ok "clock_conf"; else bad "clock_conf: $(clock_conf <<<"$meta")"; fi
meta=${meta}$'update: id:0 key:\'clock.force-quantum\' value:\'256\' type:\'\'\n'
if [[ $(clock_conf <<<"$meta") == 48000/256 ]]; then ok "clock_conf forced"; else bad "clock_conf forced: $(clock_conf <<<"$meta")"; fi
if [[ $(echo none | clock_conf) == '?/?' ]]; then ok "clock_conf unknown"; else bad "clock_conf unknown"; fi

echo "RESULT failed=$fail"
exit "$fail"
