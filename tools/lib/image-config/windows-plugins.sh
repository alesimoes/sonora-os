# Sourced by tools/test-image-config.sh: Windows plugins through Wine and
# yabridge in the image (ADR-0017 Y6). The version and checksum pins must
# agree between the hook and the container test, and the prefix setup must
# never touch the engine's real-time budget.
YB_HOOK="$ROOT/iso/config/hooks/normal/0200-sonora-yabridge.hook.chroot"
YB_LIST="$ROOT/iso/config/package-lists/windows-plugins.list.chroot"
YB_UNIT="$INC/etc/systemd/system/sonora-windows-plugins.service"
YB_SCRIPT="$INC/usr/local/bin/sonora-windows-plugins"
YB_DOCKER="$ROOT/tools/Dockerfile.yabridge"
for f in "$YB_HOOK" "$YB_LIST" "$YB_UNIT" "$YB_SCRIPT" "$YB_DOCKER"; do
  [ -f "$f" ] || bad "missing ${f#"$ROOT"/}"
done
if [ -f "$YB_HOOK" ] && [ -f "$YB_DOCKER" ]; then
  for k in VERSION SHA256; do
    h="$(sed -nE "s/^SONORA_YABRIDGE_$k=([^[:space:]]+)$/\1/p" "$YB_HOOK")"
    d="$(sed -nE "s/^ARG YABRIDGE_$k=([^[:space:]]+)$/\1/p" "$YB_DOCKER")"
    [ -n "$h" ] || bad "yabridge hook: no SONORA_YABRIDGE_$k pin"
    [ "$h" = "$d" ] || bad "yabridge $k: hook '$h' differs from Dockerfile.yabridge '$d'"
  done
  h="$(sed -nE 's/^SONORA_YABRIDGE_SHA256=(.*)$/\1/p' "$YB_HOOK")"
  printf '%s' "$h" | grep -Eq '^[0-9a-f]{64}$' || bad "yabridge hook: SHA-256 pin is not 64 hex digits"
  grep -q 'sha256sum -c' "$YB_HOOK" || bad "yabridge hook does not verify the tarball"
  grep -Eq 'SONORA_WINE_SERIES=[0-9]+\.[0-9]+' "$YB_HOOK" || bad "yabridge hook does not pin the Wine series"
  grep -q 'chmod 0755 /usr/local/bin/sonora-windows-plugins' "$YB_HOOK" || bad "yabridge hook does not chmod the setup script"
  grep -q 'systemctl enable sonora-windows-plugins.service' "$YB_HOOK" || bad "yabridge hook does not enable the setup unit"
fi
if [ -f "$YB_LIST" ]; then
  grep -Eqx 'wine64' "$YB_LIST" || bad "windows-plugins list lacks wine64"
  grep -Eqx 'wine32|libwine:i386' "$YB_LIST" && bad "windows-plugins list pulls 32-bit Wine"
fi
if [ -f "$YB_UNIT" ]; then
  grep -Eqx 'User=sonora' "$YB_UNIT" || bad "windows-plugins unit does not run as the operator"
  grep -Eq '^Type=oneshot' "$YB_UNIT" || bad "windows-plugins unit is not oneshot"
  grep -Eq '^(Before|Requires|RequiredBy)=.*sonora-session' "$YB_UNIT" && bad "windows-plugins unit is ordered against the engine"
  grep -Eq '^LimitRTPRIO' "$YB_UNIT" && bad "windows-plugins unit asks for real time"
fi
if [ -f "$YB_SCRIPT" ]; then
  grep -q 'yabridgectl sync' "$YB_SCRIPT" || bad "sonora-windows-plugins does not run yabridgectl sync"
  grep -q 'wineboot --init' "$YB_SCRIPT" || bad "sonora-windows-plugins does not create the prefix"
  grep -v "^[[:space:]]*#" "$YB_SCRIPT" | grep -q "set --path" && bad "script uses yabridgectl set --path (panics in 5.1.1)"
  # Behaviour, without Wine: --check changes nothing and never fails.
  chk_home="$(mktemp -d)"
  out="$(HOME="$chk_home" PATH="$chk_home/bin:$PATH" sh -c '
    mkdir -p "$HOME/bin"; for t in wineboot yabridgectl; do printf "#!/bin/sh\nexit 0\n" > "$HOME/bin/$t"; chmod +x "$HOME/bin/$t"; done
    sh "'"$YB_SCRIPT"'" --check "$HOME/p"' 2>&1)" || bad "sonora-windows-plugins --check failed: $out"
  printf '%s\n' "$out" | grep -q 'would be created' || bad "--check does not report the missing prefix: $out"
  [ -e "$chk_home/.wine" ] && bad "--check created the prefix"
  rm -rf "$chk_home"
fi
