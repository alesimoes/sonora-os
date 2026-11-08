# Sourced by tools/test-image-config.sh: the operator never reaches a login
# prompt or a shell. The session owns tty1, no getty is left anywhere, the
# display fallback always ends on the session, and the terminal UI owns tty2.

# --- no display manager, no login prompt, never a shell ---
grep -Eqx 'gdm3|lightdm|sddm|lxdm|slim|nodm|xdm|greetd|task-.*desktop' "$PKGS" && bad "package list installs a display manager"
grep -Eq '^ExecStart=/usr/local/bin/sonora-engine( |$)' "$UNIT" || bad "unit does not start the engine"
grep -qx 'Restart=always' "$UNIT" || bad "unit: engine is not restarted on every exit"
grep -qx 'StartLimitIntervalSec=0' "$UNIT" || bad "unit: a start limit would leave tty1 blank after a crash loop"
grep -qx 'TTYPath=/dev/tty1' "$UNIT" || bad "unit does not own tty1"
grep -qx 'Conflicts=getty@tty1.service' "$UNIT" || bad "unit does not displace the tty1 getty"
grep -qx 'User=sonora' "$UNIT" || bad "unit does not run as the operator"
grep -qx 'WantedBy=multi-user.target' "$UNIT" || bad "unit is not part of the default target"
grep -qx 'NAutoVTs=0' "$LOGIND" || bad "logind spawns VT gettys: Ctrl+Alt+F2 is a passwordless login"
grep -qx 'ReserveVT=0' "$LOGIND" || bad "logind reserves a VT with a getty on it"
grep -qx 'systemctl enable sonora-session.service' "$HOOK" || bad "hook does not enable the session unit"
grep -q 'systemctl disable getty@tty1.service' "$HOOK" || bad "hook leaves the tty1 getty enabled"
grep -q 'systemctl set-default multi-user.target' "$HOOK" || bad "hook does not boot to multi-user.target"
grep -q 'display-manager.service' "$HOOK" || bad "hook does not refuse an installed display manager"

# NAutoVTs=0 only stops logind's on-demand gettys. An enabled getty@tty2..6
# (or autovt@) would still put a passwordless login behind Ctrl+Alt+F2..F6,
# whatever logind.conf says, so nothing in the tree may start one.
vt='(getty|autovt)@tty[2-6]'
# In a hook, any non-comment mention other than a mask or disable counts:
# systemctl enable, a hand-made getty.target.wants symlink, a sed on NAutoVTs.
grep -EHn "$vt" "$ROOT"/iso/config/hooks/*/*.hook.* 2>/dev/null |
  grep -Ev '^[^:]*:[0-9]+:[[:space:]]*#' |
  grep -Ev "systemctl (mask|disable) ([^ ]+ )*$vt" &&
  bad "a hook enables a VT getty on tty2..6"
unit_dirs=()
for d in "$INC/etc/systemd" "$INC/usr/lib/systemd" "$INC/lib/systemd"; do
  [ -d "$d" ] && unit_dirs+=("$d")
done
# grep exits 2 on a missing directory even after a match, hence the list.
grep -EHnr "^(Wants|Requires|Upholds|BindsTo|Also|WantedBy|RequiredBy|UpheldBy)=.*$vt" "${unit_dirs[@]}" &&
  bad "a unit pulls in a VT getty on tty2..6"
while IFS= read -r f; do
  # A symlink to /dev/null is a mask, which is the safe direction.
  [ "$(readlink "$f" 2>/dev/null)" = /dev/null ] && continue
  bad "includes.chroot ships ${f#"$ROOT"/}"
done < <(find "$INC" \( -name 'getty@tty[2-6].service' -o -name 'autovt@tty[2-6].service' \) 2>/dev/null)

# --- display fallback (ADR-0010 stage 1): it must always end on the session ---
flag=/run/sonora-display-failed
if [ -f "$FALLBACK" ]; then
  # Without '-' a failed stop ends the oneshot before the session starts, and
  # tty1 is left with neither the display nor the engine.
  grep -Eq '^ExecStart=-/usr/bin/systemctl stop ' "$FALLBACK" || bad "fallback: a failed stop would skip starting the session (needs '-')"
  grep -Eq '^ExecStart=/usr/bin/systemctl stop ' "$FALLBACK" && bad "fallback: a systemctl stop without '-'"
  grep -qx "ExecStart=/usr/bin/touch $flag" "$FALLBACK" || bad "fallback does not create $flag"
  grep -Eq '^ExecStart=-?/usr/bin/systemctl (--no-block )?start sonora-session\.service$' "$FALLBACK" || bad "fallback does not start sonora-session"
  grep -qx "ConditionPathExists=|$flag" "$UNIT" || bad "session condition does not accept $flag"
  grep -qx 'ConditionKernelCommandLine=|!sonora.gui' "$UNIT" || bad "session condition is not '|!sonora.gui'"
  # systemd 257 runs OnFailure= on every failure, restarted or not; without
  # this guard a single kill -9 of the UI ends the display for the boot.
  grep -qx 'ExecCondition=/usr/local/bin/sonora-display-gave-up' "$FALLBACK" || bad "fallback runs on a failure that systemd is about to restart (no ExecCondition=sonora-display-gave-up)"
  grep -qx 'StartLimitIntervalSec=0' "$FALLBACK" || bad "fallback has a start limit; its skipped runs would use it up before the one that matters"
  grep -q 'auto-restart' "$INC/usr/local/bin/sonora-display-gave-up" 2>/dev/null || bad "sonora-display-gave-up does not look at auto-restart"
  grep -q 'chvt 2' "$FALLBACK" && bad "fallback switches to tty2, which has no terminal UI before ADR-0010 stage 2"
fi

# --- the terminal UI on tty2 (ADR-0010 stage 2) ---
TUIU="$INC/etc/systemd/system/sonora-tui.service"
if [ ! -f "$TUIU" ]; then
  bad "missing sonora-tui.service"
else
  grep -qx 'ExecStart=/usr/local/bin/sonora-tui' "$TUIU" || bad "tui unit does not run sonora-tui"
  grep -qx 'TTYPath=/dev/tty2' "$TUIU" || bad "tui unit is not on tty2"
  grep -qx 'Conflicts=getty@tty2.service' "$TUIU" || bad "tui unit does not displace the tty2 getty"
  grep -qx 'Restart=always' "$TUIU" || bad "tui unit does not restart"
  grep -qx 'StandardInput=tty' "$TUIU" || bad "tui unit has no tty input"
  grep -qx 'ConditionKernelCommandLine=sonora.gui' "$TUIU" || bad "tui unit runs without the display stack"
  grep -qx 'ConditionKernelCommandLine=!sonora.install' "$TUIU" || bad "tui unit fights the installer for tty2"
  grep -qx 'Wants=sonora-engine.service' "$TUIU" || bad "tui unit does not pull in the engine"
  grep -qx 'systemctl enable sonora-tui.service' "$HOOK" || bad "hook does not enable the tui unit"
fi
