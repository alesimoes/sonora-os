# Sourced by tools/test-image-config.sh: the display stack (ADR-0010). The
# boot smoke test, the compositor and UI units, the engine as its own unit,
# and the QEMU modes that reach the smoke test's display checks.

# --- boot smoke test: one orderly poweroff on success ---
# A forced poweroff on success skips the unit stops, so the persistence
# overlay is never unmounted and test-write-usb.sh's clean-fsck check after
# boot 3 measures luck. A poweroff in the script races the unit's action.
SMOKE_UNIT="$INC/etc/systemd/system/sonora-smoketest.service"
SMOKE_SCRIPT="$INC/usr/local/bin/sonora-smoketest"
if [ -f "$SMOKE_UNIT" ]; then
  # [Unit] keys only: under [Service] systemd ignores them with a warning.
  smoke_unit_keys="$(awk '/^\[/ { u = ($0 == "[Unit]") } u' "$SMOKE_UNIT")"
  printf '%s\n' "$smoke_unit_keys" | grep -qx 'SuccessAction=poweroff' ||
    bad "smoketest unit: [Unit] SuccessAction= is not an orderly 'poweroff'"
  printf '%s\n' "$smoke_unit_keys" | grep -qx 'FailureAction=poweroff-force' ||
    bad "smoketest unit: [Unit] FailureAction= is not 'poweroff-force'; a wedged check would hold the guest up"
  grep -Ev '^[[:space:]]*#' "$SMOKE_SCRIPT" | grep -Eq '(systemctl[[:space:]].*(poweroff|halt|reboot)|^[[:space:]]*(poweroff|halt|reboot|shutdown)([[:space:]]|$))' &&
    bad "sonora-smoketest powers off by itself; the unit's SuccessAction= does that"
fi

# --- display stack staged into the image (ADR-0010 stage 1) ---
# Each is a way sonora.gui boots to a blank screen with no build error.
for p in libpam-systemd labwc xwayland qt6-wayland libqt6quick6 qml6-module-qtquick; do
  grep -qx "$p" "$PKGS" || bad "package list lacks $p (display block)"
done
# seatd would satisfy libseat1 and give labwc no logind session.
grep -Eqx 'seatd|libseat1-seatd' "$PKGS" && bad "package list installs seatd; ADR-0010 section 3 wants logind"
for f in usr/share/sonora/labwc/rc.xml usr/share/sonora/labwc/menu.xml etc/pam.d/sonora-ui \
         usr/local/bin/sonora-compositor usr/local/bin/sonora-wait-for-display \
         etc/systemd/system/sonora-compositor.service etc/systemd/system/sonora-ui.service; do
  [ -f "$INC/$f" ] || bad "includes.chroot lacks $f"
done
for b in sonora-compositor sonora-wait-for-display sonora-display-gave-up; do
  grep -qx "chmod 0755 /usr/local/bin/$b" "$HOOK" || bad "hook does not chmod $b"
done
grep -qx 'systemctl enable sonora-compositor.service' "$HOOK" || bad "hook does not enable the compositor unit"
grep -q 'sonora-ui' "$HOOK" || bad "hook does not handle sonora-ui"
COMP="$INC/etc/systemd/system/sonora-compositor.service"
UIU="$INC/etc/systemd/system/sonora-ui.service"
if [ -f "$COMP" ] && [ -f "$UIU" ]; then
  grep -qx 'ConditionKernelCommandLine=sonora.gui' "$COMP" || bad "compositor unit is not gated on sonora.gui"
  grep -qx 'ConditionKernelCommandLine=sonora.gui' "$UIU" || bad "UI unit is not gated on sonora.gui"
  grep -qx 'PAMName=sonora-ui' "$COMP" || bad "compositor unit has no PAM session (no logind seat)"
  grep -qx 'Conflicts=getty@tty1.service' "$COMP" || bad "compositor unit does not displace the tty1 getty"
  grep -qx 'OnFailure=sonora-display-fallback.service' "$COMP" || bad "compositor failure does not start the fallback"
  grep -qx 'OnFailure=sonora-display-fallback.service' "$UIU" || bad "UI failure does not start the fallback"
  grep -qx 'BindsTo=sonora-compositor.service' "$UIU" || bad "UI unit is not bound to the compositor"
  grep -qx 'RestartPreventExitStatus=3' "$UIU" || bad "UI unit retries the 'unfit display' exit"
fi

# --- kiosk stage 2: the engine as its own unit, editors on labwc ---
# The engine and the display stack must fail and restart separately: a
# compositor or UI crash that restarts the engine ends the take in progress.
ENG="$INC/etc/systemd/system/sonora-engine.service"
RUN="$INC/usr/local/bin/sonora-engine-run"
for f in "$ENG" "$RUN" "$INC/usr/share/sonora/labwc/autostart"; do
  [ -f "$f" ] || bad "kiosk stage 2 lacks ${f#"$INC"/}"
done
if [ -f "$ENG" ] && [ -f "$UIU" ]; then
  grep -qx 'ConditionKernelCommandLine=sonora.gui' "$ENG" || bad "engine unit is not gated on sonora.gui"
  grep -qx 'ConditionPathExists=!/run/sonora-display-failed' "$ENG" || bad "engine unit does not stand down after the display fallback"
  # Not Conflicts=: the session is enabled at boot, and a Conflicts= between two
  # units in one boot transaction made systemd drop the session (measured in
  # tools/test-display-fallback.sh). The fallback stops the engine by hand.
  grep -Eq '^Conflicts=.*sonora-(session|engine)' "$ENG" "$UNIT" && bad "Conflicts= between the engine and the session unit"
  grep -Eq '^ExecStart=-/usr/bin/systemctl stop .*sonora-engine\.service' "$FALLBACK" || bad "fallback does not stop the display stack's engine"
  grep -qx 'Restart=always' "$ENG" || bad "engine unit does not restart"
  grep -qx 'StartLimitIntervalSec=0' "$ENG" || bad "engine unit gives up after a crash loop"
  grep -qx 'LimitRTPRIO=95' "$ENG" || bad "engine unit has no RT priority grant"
  grep -qx 'LimitMEMLOCK=infinity' "$ENG" || bad "engine unit has no memlock grant"
  grep -qx 'ExecStart=/usr/local/bin/sonora-engine-run' "$ENG" || bad "engine unit does not start the launcher"
  grep -Eq '^(BindsTo|Requires|PartOf)=' "$ENG" && bad "engine unit is tied to another unit's stop; a UI or labwc crash would end the take"
  grep -qx 'Wants=sonora-engine.service' "$UIU" || bad "UI unit does not pull in the engine"
  grep -qx 'After=sonora-engine.service' "$UIU" || bad "UI unit is not ordered after the engine"
  grep -Eq '^(BindsTo|Requires)=.*sonora-engine' "$UIU" && bad "UI unit is bound to the engine; the engine's restart would restart the UI"
  grep -Eq '^ExecStart=/usr/local/bin/sonora-ui --kiosk$' "$UIU" || bad "UI unit does not run the real-engine kiosk (--demo left in?)"
  grep -qx 'Restart=always' "$UIU" || bad "UI unit does not restart"
fi
grep -qx 'chmod 0755 /usr/local/bin/sonora-engine-run' "$HOOK" || bad "hook does not chmod sonora-engine-run"
grep -qx 'chmod 0755 /usr/share/sonora/labwc/autostart' "$HOOK" || bad "hook does not chmod the labwc autostart"
# The engine is pulled in by the UI; enabling it would start it without sonora.gui checks.
grep -q 'systemctl enable sonora-engine.service' "$HOOK" && bad "hook enables the engine unit; the UI unit pulls it in"
grep -q 'display.env' "$RUN" 2>/dev/null || bad "engine launcher does not read the display file"
grep -q 'display.env' "$INC/usr/share/sonora/labwc/autostart" 2>/dev/null || bad "labwc autostart does not write the display file"
grep -Eq -- '--headless' "$RUN" 2>/dev/null || bad "engine launcher is not headless"
RC="$INC/usr/share/sonora/labwc/rc.xml"
# Editors are every window but the UI: no rule may strip their title bar, the
# operator's only way to move or close a misbehaving one.
if [ -f "$RC" ]; then
  grep -c 'serverDecoration="no"' "$RC" | grep -qx 1 || bad "rc.xml undecorates more than one window rule"
  grep -q 'windowRule identifier="sonora-ui" serverDecoration="no"' "$RC" || bad "rc.xml: the only undecorated window is not sonora-ui"
  grep -q '<policy>center</policy>' "$RC" || bad "rc.xml: editors are not centred"
  grep -q '<repeatRate>' "$RC" || bad "rc.xml sets no key repeat"
fi
grep -q 'XKB_DEFAULT_LAYOUT' "$INC/usr/local/bin/sonora-compositor" || bad "compositor wrapper does not map the keyboard layout"

grep -q '^session .*pam_systemd.so' "$INC/etc/pam.d/sonora-ui" 2>/dev/null || bad "pam sonora-ui does not open a logind session"
grep -Ev '^[[:space:]]*#' "$INC/etc/pam.d/sonora-ui" 2>/dev/null | grep -q pam_limits && bad "pam sonora-ui loads pam_limits; the UI would inherit rtprio 95"

# sonora-ui is built by tools/build-ui.sh and staged on request.
grep -q 'SONORA_WITH_UI' "$BUILD_ISO" || bad "build-iso.sh cannot stage sonora-ui"
# shellcheck disable=SC2016 # a literal $UI in the script text
grep -q 'install -m 0755 "\$UI" "\$STAGE/sonora-ui"' "$BUILD_ISO" || bad "build-iso.sh does not install sonora-ui into includes.chroot"

# --- the smoke test's display checks and the QEMU modes that reach them ---
# The recovery and fallback checks exist only if a boot asks for them, so a
# harness that never passes the parameter would turn them into dead code.
BOOT_QEMU="$ROOT/tools/test-boot-qemu.sh"
if [ -f "$SMOKE_SCRIPT" ]; then
  grep -q 'first frame after' "$SMOKE_SCRIPT" || bad "smoketest does not look for the UI's first-frame line"
  grep -q 'display_failure_report' "$SMOKE_SCRIPT" || bad "smoketest does not report why the display stack failed"
  grep -q 'kill_and_recover sonora-ui.service' "$SMOKE_SCRIPT" || bad "smoketest does not SIGKILL the UI"
  grep -q 'kill_and_recover sonora-compositor.service' "$SMOKE_SCRIPT" || bad "smoketest does not SIGKILL labwc"
  # shellcheck disable=SC2016 # literal text of the script
  grep -qx 'RECOVER_MS=\$((3000 \* SLOW))' "$SMOKE_SCRIPT" || bad "smoketest recovery limit is not ADR-0010's 3 s (times sonora.slow, 1 by default)"
  grep -q '^SLOW=1$' "$SMOKE_SCRIPT" || bad "smoketest slow multiplier does not default to 1"
  grep -q 'sonora\.break_ui' "$SMOKE_SCRIPT" || bad "smoketest cannot inject a repeated UI failure"
  grep -q 'fgconsole' "$SMOKE_SCRIPT" || bad "smoketest does not check the foreground VT after the fallback"
  grep -q 'find /dev/dri' "$SMOKE_SCRIPT" || bad "smoketest does not record the render node situation"
fi
grep -q -- '--gui)' "$BOOT_QEMU" || bad "test-boot-qemu.sh has no --gui mode"
grep -q -- '--gui-broken)' "$BOOT_QEMU" || bad "test-boot-qemu.sh has no --gui-broken mode"
grep -q 'sonora.gui sonora.break_ui' "$BOOT_QEMU" || bad "--gui-broken does not pass sonora.break_ui"
grep -q 'iso size' "$BOOT_QEMU" || bad "test-boot-qemu.sh does not record the ISO size"

# TCG fallback for hosts without /dev/kvm: one shared policy, every QEMU test
# uses it, and exit codes tell skip (77) and inconclusive (3) from fail (1).
QEMU_ACCEL="$ROOT/tools/lib/qemu-accel.sh"
[ -f "$QEMU_ACCEL" ] || bad "missing tools/lib/qemu-accel.sh"
grep -q 'tcg,thread=multi' "$QEMU_ACCEL" || bad "qemu-accel.sh has no multi-thread TCG fallback"
grep -q 'QEMU_EXIT_SKIP=77' "$QEMU_ACCEL" || bad "qemu-accel.sh does not define the skip exit code"
for t in test-boot-qemu.sh test-install-qemu.sh test-write-usb.sh; do
  grep -q 'lib/qemu-accel.sh' "$ROOT/tools/$t" || bad "$t does not use tools/lib/qemu-accel.sh"
  grep -q 'qemu_scale' "$ROOT/tools/$t" || bad "$t does not scale its timeouts for TCG"
done
