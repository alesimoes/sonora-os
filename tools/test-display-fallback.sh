#!/usr/bin/env bash
# The ADR-0010 stage 1 recovery path, run by a real systemd, without QEMU.
#
# The image's own compositor, UI, fallback and session units boot as PID 1 in
# a container, with stub binaries behind them. Three boots:
#   plain       no sonora.gui: the session owns tty1, the compositor is skipped
#   gui         sonora.gui, healthy UI: the display runs, the session stands down
#   gui-broken  sonora.gui, the UI exits 3 ("unfit display"): the fallback must
#               create the flag, stop the display and start the session
# plus `systemd-analyze verify` of every unit the image ships.
#
# What this cannot show: VT ownership (a container has no tty1, so a drop-in
# strips the TTY settings), logind seats, labwc itself. Those stay with the
# QEMU check. Needs Docker that can run systemd (--privileged).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE=sonora-systemd-test:trixie
export MSYS_NO_PATHCONV=1
src="$ROOT"
command -v cygpath >/dev/null 2>&1 && src="$(cygpath -w "$ROOT")"

docker build -q --label org.sonora=1 -t "$IMAGE" - >/dev/null <<'EOF'
FROM debian:trixie
RUN apt-get update \
 && apt-get install -y --no-install-recommends systemd systemd-sysv dbus kbd procps \
 && rm -rf /var/lib/apt/lists/*
EOF

# Runs inside the container before systemd. $1 is the UI stub's behaviour.
# shellcheck disable=SC2016 # expanded by the container's sh, not here
setup='
set -e
ui="$1"; shift
U=/src/iso/config/includes.chroot/etc/systemd/system
useradd -u 1000 -m -U sonora
cp "$U"/sonora-compositor.service "$U"/sonora-ui.service \
   "$U"/sonora-display-fallback.service "$U"/sonora-session.service \
   "$U"/sonora-smoketest.service /etc/systemd/system/
# The index has them 100644; a Windows bind mount shows every file executable.
chmod 644 /etc/systemd/system/sonora-*.service
stub() { printf "#!/bin/sh\n%s\n" "$2" > "/usr/local/bin/$1"; chmod 755 "/usr/local/bin/$1"; }
stub sonora-compositor "exec sleep infinity"
stub sonora-wait-for-display "exit 0"
# The real one: the fallback unit runs it as ExecCondition=.
cp /src/iso/config/includes.chroot/usr/local/bin/sonora-display-gave-up /usr/local/bin/
chmod 755 /usr/local/bin/sonora-display-gave-up
stub sonora-wait-for-audio "exit 0"
stub sonora-engine "exec sleep infinity"
stub sonora-smoketest "exit 0"
case "$ui" in
  broken) stub sonora-ui "exit 3" ;;
  failing) stub sonora-ui "exit 1" ;;
  *) stub sonora-ui "exec sleep infinity" ;;
esac
if [ "$ui" = verify ]; then
  exec systemd-analyze verify --man=no /etc/systemd/system/sonora-*.service
fi
# No VT and no sonora-ui PAM stack in a container; everything else is as shipped.
for u in sonora-compositor sonora-session; do
  mkdir -p "/etc/systemd/system/$u.service.d"
  printf "[Service]\nPAMName=\nTTYPath=\nTTYReset=no\nTTYVHangup=no\nTTYVTDisallocate=no\nStandardInput=null\nStandardOutput=journal\nUtmpIdentifier=\n" \
    > "/etc/systemd/system/$u.service.d/container.conf"
done
systemctl enable sonora-compositor.service sonora-session.service >/dev/null 2>&1
exec /sbin/init "$@"
'

# set -e or an interrupt can leave a scenario half way; a privileged systemd
# container left behind keeps running until someone notices. tools/clean.sh
# finds anything that escapes this by the label.
cid=
trap '[ -z "$cid" ] || docker rm -f "$cid" >/dev/null 2>&1 || true' EXIT
trap 'exit 130' INT TERM

fail=0
bad() { echo "FAIL [$scenario]: $*" >&2; fail=1; }

echo "--- systemd-analyze verify"
if ! out="$(docker run --rm --label org.sonora=1 -v "$src:/src:ro" "$IMAGE" sh -c "$setup" sh verify 2>&1)"; then
  scenario=verify bad "systemd-analyze verify"
fi
# verify also reports on stock units it loads; only ours matter here.
if printf '%s\n' "$out" | grep -i 'sonora' ; then
  scenario=verify bad "systemd-analyze verify reports the units above"
fi

# state UNIT: active, inactive, failed ...
state() { docker exec "$cid" systemctl is-active "$1" 2>/dev/null || true; }
# wait_for UNIT STATE: up to 30 s.
wait_for() {
  local i=0
  while [ "$i" -lt 30 ]; do
    [ "$(state "$1")" = "$2" ] && return 0
    i=$((i + 1))
    sleep 1
  done
  return 1
}
expect() { local s; s="$(state "$1")"; [ "$s" = "$2" ] || bad "$1 is $s, expected $2"; }
flag() { docker exec "$cid" test -e /run/sonora-display-failed; }

# recover UNIT: SIGKILL its main process inside the container and print the
# milliseconds until it runs again under a new PID; fails if that takes over
# 3 s. The same rule as sonora-smoketest's kill_and_recover.
# shellcheck disable=SC2016 # expanded by the container's sh
recover_sh='
u="$1"
old="$(systemctl show -p MainPID --value "$u")"
[ -n "$old" ] && [ "$old" != 0 ] || exit 1
t0="$(date +%s%N)"
kill -9 "$old"
while :; do
  p="$(systemctl show -p MainPID --value "$u")"
  if [ "$p" != 0 ] && [ "$p" != "$old" ] && systemctl is-active --quiet "$u"; then
    echo $(( ($(date +%s%N) - t0) / 1000000 ))
    exit 0
  fi
  [ $(( ($(date +%s%N) - t0) / 1000000 )) -ge 3000 ] && exit 1
  sleep 0.1
done'
recover() {
  docker exec "$cid" sh -c "$recover_sh" sh "$1"
}

run() {
  scenario="$1"
  local ui="$2"
  shift 2
  echo "--- $scenario"
  cid="$(docker run -d --label org.sonora=1 --privileged --cgroupns=host -v /sys/fs/cgroup:/sys/fs/cgroup:rw \
    --tmpfs /run --tmpfs /run/lock -v "$src:/src:ro" "$IMAGE" sh -c "$setup" sh "$ui" "$@")"
  case "$scenario" in
    plain)
      wait_for sonora-session.service active || bad "session never became active"
      expect sonora-compositor.service inactive
      flag && bad "fallback flag exists on a plain boot"
      ;;
    gui)
      wait_for sonora-ui.service active || bad "UI never became active"
      sleep 3
      expect sonora-compositor.service active
      expect sonora-ui.service active
      expect sonora-session.service inactive
      flag && bad "fallback ran with a healthy UI"
      ;;
    gui-kill)
      wait_for sonora-ui.service active || bad "UI never became active"
      sleep 2
      ms="$(recover sonora-ui.service)" || bad "UI did not come back within 3 s of kill -9"
      echo "kill -9 of the UI: back after ${ms:-?} ms"
      sleep 2
      ms="$(recover sonora-compositor.service)" || bad "compositor did not come back within 3 s of kill -9"
      echo "kill -9 of the compositor: back after ${ms:-?} ms"
      wait_for sonora-ui.service active || bad "UI did not follow the compositor back"
      expect sonora-session.service inactive
      flag && bad "fallback ran after two SIGKILLs"
      ;;
    gui-broken|gui-failing)
      wait_for sonora-session.service active || bad "session never took over after the UI failed"
      flag || bad "fallback flag /run/sonora-display-failed missing"
      expect sonora-compositor.service inactive
      if [ "$scenario" = gui-failing ]; then
        # Counted from the journal: NRestarts is reset when the limit trips.
        n="$(docker exec "$cid" journalctl -b -u sonora-ui.service --no-pager 2>/dev/null | grep -c 'Scheduled restart job')"
        [ "${n:-0}" -ge 3 ] || bad "the failing UI was restarted ${n:-0} times; this did not test a repeated failure"
      fi
      expect sonora-ui.service failed
      docker exec "$cid" pgrep -u sonora -x sleep >/dev/null || bad "no engine stub running"
      ;;
  esac
  if [ "$fail" -ne 0 ]; then
    docker exec "$cid" systemctl --no-pager status 'sonora-*' 2>&1 | tail -40 >&2 || true
    docker exec "$cid" journalctl -b --no-pager -u 'sonora-*' 2>&1 | tail -40 >&2 || true
  fi
  docker rm -f "$cid" >/dev/null
  cid=
}

run plain healthy
run gui healthy sonora.gui
run gui-broken broken sonora.gui
run gui-kill healthy sonora.gui
# Exit 1 is retried (only 3 is not): five restarts in 60 s trip the start
# limit, and only then does OnFailure= hand tty1 to the session.
run gui-failing failing sonora.gui

[ "$fail" -eq 0 ] && echo "ok: display fallback hands tty1 back to sonora-session; units verify"
exit "$fail"
