#!/usr/bin/env bash
# The parts of tools/dev-wsl.sh that need neither WSL nor root: the wsl.conf
# edit of the `user` step, the session-bus liveness check, the Windows log
# path and the --log wrapper. Runs on any Linux (CI's "Shell scripts" job).
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
once="$(printf '[boot]\nsystemd=true' | wsl_conf_set_user sonora)"
twice="$(printf '%s\n' "$once" | wsl_conf_set_user sonora)"
if [[ $once == "$twice" ]]; then ok "wsl.conf edit is idempotent"; else bad "wsl.conf second edit differs"; fi

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
printf '%s\n' "$ovr" "$ovr" >>"$STATE/pipewire.log"
if tunnel_dead; then ok "sink buffer full, nothing reads it: dead"; else bad "stalled sink tunnel not seen"; fi
printf '%s\n' '[E][x] mod.pulse-tunnel | connection failure' "$ovr" >"$STATE/pipewire.log"
if tunnel_dead; then ok "connection failure under overrun lines: dead"; else bad "connection failure missed"; fi

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

echo "RESULT failed=$fail"
exit "$fail"
