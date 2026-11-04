#!/usr/bin/env bash
# The tty2 text front end against a stub installer: no disk, no root, no
# container. It checks what the screen promises the operator -- nothing is
# started before the typed confirmation, progress shows STEP n/7, each exit
# code gets a named reason, and a failed install offers a retry instead of
# ending -- and that the script is handed exactly the chosen disk.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TTY="$ROOT/iso/config/includes.chroot/usr/local/bin/sonora-install-tty"
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
fails=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fails=$((fails + 1)); }

cat >"$T/stub" <<'STUB'
#!/bin/sh
case $1 in
    --list) printf '/dev/nvme0n1\t512110190592\tSAMSUNG SSD\n/dev/sda\t240057409536\tCRUCIAL\n'; exit 0 ;;
esac
echo "$*" >>"$STUB_DIR/calls"
for n in 1 2 3 4 5 6 7; do echo "STEP $n/7 s$n"; done
echo "INFO carried over 3 session files"
[ "${STUB_RC:-0}" = 0 ] || echo "sonora-install: stub failure reason" >&2
exit "${STUB_RC:-0}"
STUB
chmod +x "$T/stub"
cat >"$T/reboot" <<'STUB'
#!/bin/sh
echo rebooted >>"$STUB_DIR/calls"
STUB
chmod +x "$T/reboot"
mkdir -p "$T/bin"
printf '#!/bin/sh\nexit 0\n' >"$T/bin/chvt"
chmod +x "$T/bin/chvt"

# run NAME RC INPUT: the screen with INPUT on stdin, output in $T/NAME.out
run() {
    : >"$T/calls"
    STUB_DIR=$T STUB_RC=$2 SONORA_INSTALL=$T/stub SONORA_REBOOT=$T/reboot \
        SONORA_INSTALL_LOG=$T/log PATH="$T/bin:$PATH" \
        sh "$TTY" <<<"$3" >"$T/$1.out" 2>&1 || true
}

# 1. a good install: pick 2, type the name, restart
run good 0 $'2\n/dev/sda\n\n'
if grep -q -- '--yes --target /dev/sda' "$T/calls"; then ok "script handed exactly the chosen disk"; else bad "target: $(cat "$T/calls")"; fi
if grep -q 'STEP 4/7' "$T/good.out" && grep -q 'STEP 7/7' "$T/good.out"; then ok "progress shows STEP n/7"; else bad "progress"; fi
if grep -q '^\[####---\] STEP 4/7' "$T/good.out"; then ok "progress bar fills with the step"; else bad "bar"; fi
if grep -q 'carried over 3 session files' "$T/good.out"; then ok "INFO lines shown"; else bad "INFO lines"; fi
if grep -q 'INSTALLED to /dev/sda' "$T/good.out" && grep -q '^rebooted' "$T/calls"; then ok "result, then restart"; else bad "success path"; fi
if grep -q 'SAMSUNG SSD' "$T/good.out" && grep -q '512 GB' "$T/good.out"; then ok "disks listed with size and model"; else bad "disk list"; fi

# 2. a wrong confirmation starts nothing
run cancel 0 $'1\nyes\nq\n'
if [ ! -s "$T/calls" ]; then ok "wrong confirmation runs nothing"; else bad "installer started: $(cat "$T/calls")"; fi
if grep -q 'Cancelled' "$T/cancel.out"; then ok "cancel is announced"; else bad "no cancel notice"; fi

# 3. a bad choice is asked again; the other disk is not touched
run range 0 $'7\nx\n1\n/dev/nvme0n1\n\n'
if grep -q -- '--target /dev/nvme0n1$' "$T/calls" && ! grep -q sda "$T/calls"; then ok "out-of-range and non-numeric picks re-ask"; else bad "pick validation: $(cat "$T/calls")"; fi

# 4. failures: named reason, last stderr line, retry offered, no restart
for code in 3 4 5 6 7 8 9 10 11; do
    run "fail$code" "$code" $'1\n/dev/nvme0n1\nq\n'
    if grep -q "INSTALL FAILED (code $code): [A-Z]" "$T/fail$code.out" && ! grep -q 'rebooted' "$T/calls" &&
       grep -q 'stub failure reason' "$T/fail$code.out" && grep -q 'start over' "$T/fail$code.out"; then
        ok "exit $code: reason, detail and retry offered"
    else
        bad "exit $code screen"
    fi
done
if grep -q 'on the stick are untouched' "$T/fail11.out"; then ok "carry-over failure says the stick is intact"; else bad "exit 11 wording"; fi

# 5. Enter after a failure reaches the installer again
run retry 6 $'1\n/dev/nvme0n1\n\n1\n/dev/nvme0n1\nq\n'
if [ "$(grep -c -- '--yes --target' "$T/calls")" = 2 ]; then ok "Enter after a failure starts over"; else bad "retry"; fi

# 6. no disk: says so and leaves without installing
printf '#!/bin/sh\nexit 0\n' >"$T/none"
chmod +x "$T/none"
SONORA_INSTALL=$T/none PATH="$T/bin:$PATH" sh "$TTY" <<<$'\n' >"$T/none.out" 2>&1 || true
if grep -q 'No internal disk' "$T/none.out"; then ok "no candidate disk is reported"; else bad "no-disk message"; fi

# 7. the unit is inert without sonora.install and on an installed system
UNIT="$ROOT/iso/config/includes.chroot/etc/systemd/system/sonora-installer.service"
for c in 'ConditionKernelCommandLine=sonora.install' 'ConditionPathExists=/run/live/medium' \
         'ConditionPathExists=!/var/lib/sonora/installed' 'TTYPath=/dev/tty2' 'Restart=on-failure'; do
    if grep -qxF "$c" "$UNIT"; then ok "unit has $c"; else bad "unit lacks $c"; fi
done

echo "failures: $fails"
[ "$fails" -eq 0 ]
