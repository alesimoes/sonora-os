# Sourced by tools/test-image-config.sh: every binary the image runs is in
# the image. A unit or a launcher that names /usr/local/bin/<x> for an <x>
# that nothing stages boots into a restart loop (or, for the plugin host, into
# an engine that silently has no inserts). Each such path must be a file
# tracked in includes.chroot or listed in iso/staged.list, which build-iso.sh
# stages.
STAGED="$ROOT/iso/staged.list"
BIN="$INC/usr/local/bin"
if [ ! -f "$STAGED" ]; then
  bad "missing iso/staged.list"
else
  staged_paths="$(awk '!/^[[:space:]]*(#|$)/ { print $1 }' "$STAGED")"
  required_paths="$(awk '$2 == "required" { print $1 }' "$STAGED")"
  refs="$(grep -rhEv '^[[:space:]]*#' "$INC/etc/systemd/system" "$BIN" "$HOOK" 2>/dev/null |
    grep -Eo '/usr/local/bin/[A-Za-z0-9._-]+' | sort -u || true)"
  [ -n "$refs" ] || bad "found no /usr/local/bin reference in the units and launchers: the check is blind"
  for p in $refs; do
    [ -f "$INC$p" ] && continue
    printf '%s\n' "$staged_paths" | grep -qxF "$p" || bad "$p is run by a unit or launcher but is neither tracked in includes.chroot nor in iso/staged.list"
  done
  # A listed binary needs a way in: build-iso.sh must name it, or the producer
  # it comes from (sonora-plugin-host and its siblings are installed by a loop).
  for p in $required_paths; do
    case "$p" in
      */) continue ;;
    esac
    n="$(basename "$p")"
    grep -q "$n" "$BUILD_ISO" || grep -q "$n" "$ROOT/tools/build-plugins.sh" || bad "$n is required by iso/staged.list but build-iso.sh never stages it"
  done
  for p in $required_paths; do
    case "$p" in
      /usr/local/bin/*) printf '%s\n' "$refs" | grep -qxF "$p" || [ "$p" = /usr/local/bin/sonora-tui ] || bad "$p is staged but nothing runs it" ;;
    esac
  done
  printf '%s\n' "$staged_paths" | grep -qxF /usr/local/bin/sonora-plugin-host || bad "iso/staged.list does not stage sonora-plugin-host"
  printf '%s\n' "$staged_paths" | grep -qxF /usr/local/lib/lv2/ || bad "iso/staged.list does not stage the LV2 bundles"
  printf '%s\n' "$staged_paths" | grep -qxF /usr/local/lib/vst3/ || bad "iso/staged.list does not stage the VST3 bundles"
  printf '%s\n' "$staged_paths" | grep -qxF /usr/share/sonora/keymaps/ || bad "iso/staged.list does not stage the keymap profiles"
fi
grep -q -- '--list-staged' "$BUILD_ISO" || bad "build-iso.sh has no --list-staged"
grep -q -- '--stage' "$ROOT/tools/build-plugins.sh" || bad "build-plugins.sh has no --stage mode for the image"
grep -q 'build-plugins.sh" --stage' "$BUILD_ISO" || bad "build-iso.sh does not build the plugin host and bundles"
grep -q 'usr/local/lib/lv2' "$BUILD_ISO" || bad "build-iso.sh does not stage the LV2 bundles"
grep -q 'usr/local/lib/vst3' "$BUILD_ISO" || bad "build-iso.sh does not stage the VST3 bundles"
grep -qE 'SONORA_WITH_UI:-1' "$BUILD_ISO" || bad "build-iso.sh no longer stages sonora-ui by default"
ls "$ROOT"/keymaps/*.json >/dev/null 2>&1 || bad "no keymaps/*.json to stage"
# Runtime libraries of what is staged, from the package list.
for pkg in liblilv-0-0 libx11-6 qt6-wayland libqt6quick6 qml6-module-qtquick; do
  grep -qx "$pkg" "$PKGS" || bad "package list lacks $pkg, which sonora-plugin-host or sonora-ui needs at run time"
done
