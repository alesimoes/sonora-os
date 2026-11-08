# Sourced by tools/test-image-config.sh: audio-stack/sysctl-sonora.conf.
# systemd-sysctl logs a bad line and goes on booting, and the kernel quietly
# rewrites some combinations, so each of these leaves the default in force
# with nothing on screen.
SYSCTL="$ROOT/audio-stack/sysctl-sonora.conf"
SYSCTL_IMG="$INC/usr/share/sonora/sysctl-sonora.conf"
declare -A sc=()
if [ ! -f "$SYSCTL" ]; then
  bad "missing audio-stack/sysctl-sonora.conf"
else
  while IFS= read -r line || [ -n "$line" ]; do
    case "$line" in ''|'#'*|';'*) continue ;; esac
    if [[ "$line" =~ ^([a-z0-9_]+(\.[a-z0-9_]+)+)[[:space:]]*=[[:space:]]*([^[:space:]]+)[[:space:]]*$ ]]; then
      k="${BASH_REMATCH[1]}"
      [ -n "${sc[$k]:-}" ] && bad "sysctl: $k set twice; only the last one counts"
      sc[$k]="${BASH_REMATCH[3]}"
    else
      bad "sysctl: line systemd-sysctl cannot parse: '$line'"
    fi
  done < "$SYSCTL"
  # int_in KEY LO HI: KEY is set to an integer in [LO, HI].
  int_in() {
    local v="${sc[$1]:-}"
    [[ "$v" =~ ^[0-9]+$ ]] && [ "$v" -ge "$2" ] && [ "$v" -le "$3" ] ||
      bad "sysctl: $1='$v', expected an integer in $2..$3"
  }
  int_in vm.swappiness 0 200
  int_in vm.dirty_background_ratio 1 100
  int_in vm.dirty_ratio 1 100
  int_in fs.inotify.max_user_watches 8192 4194304
  # Background at or above the hard limit is halved by the kernel without a word.
  bgr="${sc[vm.dirty_background_ratio]:-}" dr="${sc[vm.dirty_ratio]:-}"
  [[ "$bgr" =~ ^[0-9]+$ && "$dr" =~ ^[0-9]+$ ]] && [ "$bgr" -ge "$dr" ] &&
    bad "sysctl: dirty_background_ratio $bgr is not below dirty_ratio $dr; the kernel uses $dr/2"
  # Writing a *_bytes knob zeroes its *_ratio twin, whichever line comes later.
  for k in vm.dirty_bytes vm.dirty_background_bytes; do
    [ -n "${sc[$k]:-}" ] && bad "sysctl: $k overrides the ratio it pairs with"
  done
  # A misspelt key is a boot-time warning nobody reads. Checked against the
  # running kernel's /proc/sys, which CI's Linux runner has.
  if [ -d /proc/sys/vm ]; then
    for k in "${!sc[@]}"; do
      [ -e "/proc/sys/${k//.//}" ] || bad "sysctl: $k is not a kernel parameter here (/proc/sys/${k//.//})"
    done
  else
    echo "note: no /proc/sys/vm on this host; sysctl key names not checked against a kernel"
  fi
  cmp -s "$SYSCTL" "$SYSCTL_IMG" || bad "sysctl: the image's copy differs from audio-stack/ (tools/sync-audio-stack.sh)"
fi
# systemd-sysctl reads only *.conf in sysctl.d, and a later name wins per key.
grep -Eq '^[[:space:]]*/etc/sysctl\.d/99-[a-z0-9-]+\.conf$' "$HOOK" ||
  bad "hook: sysctl-sonora.conf not installed as /etc/sysctl.d/99-*.conf"
grep -Eq 'install .*/usr/share/sonora/sysctl-sonora\.conf' "$HOOK" || bad "hook does not install sysctl-sonora.conf"
for d in "$INC/etc/sysctl.d" "$INC/usr/lib/sysctl.d" "$INC/lib/sysctl.d"; do
  [ -d "$d" ] || continue
  for f in "$d"/*.conf; do
    [ -e "$f" ] || continue
    for k in "${!sc[@]}"; do
      grep -Eq "^[[:space:]]*-?${k//./[./]}[[:space:]]*=" "$f" && bad "${f#"$ROOT"/} also sets $k"
    done
  done
done
