# Sourced by tools/test-image-config.sh: the kernel flavour and the package
# list. Each line is a way the image installs the wrong thing with no error.

# --- RT kernel ---
grep -Eq -- '--linux-flavours[[:space:]]+rt-amd64' "$AUTO" || bad "auto/config: RT kernel is not the bootstrap flavour"
grep -Eq '^linux-image' "$PKGS" && bad "package list names a kernel; it would pull the generic one in"

# --- firmware: listed by name, nothing fetched from outside the snapshot ---
# live-build's firmware auto-inclusion (--firmware-chroot, on by default) reads
# the archive's Contents files and installs every package with a file under
# lib/firmware, contrib included. That is how firmware-b43-installer reached
# the chroot whatever the package list said: its postinst downloads the
# Broadcom blob from github.com while the image builds, and a 504 there failed
# the nightly (2026-11-08). --firmware-binary is the same for the installer.
for opt in --firmware-chroot --firmware-binary; do
  grep -Eq -- "$opt[[:space:]]+false" "$AUTO" ||
    bad "auto/config: $opt is not false; live-build would install every archive firmware package, b43 installers included"
done
grep -Eq '^firmware-b43[a-z]*-installer$' "$PKGS" &&
  bad "package list names a b43 installer, whose postinst downloads during the build"
grep -Eq '^firmware-linux(-nonfree)?$' "$PKGS" &&
  bad "package list names a firmware metapackage; the shipped firmware is listed by name, not left to its dependency set"
grep -Eq '^firmware-' "$PKGS" || bad "package list ships no firmware, and auto-inclusion is off"

# --- audio stack ---
grep -Eq '^libjack-jackd2-0$|^jackd' "$PKGS" && bad "package list installs a libjack that shadows pipewire-jack"
for p in pipewire pipewire-jack wireplumber libspa-0.2-modules; do
  grep -qx "$p" "$PKGS" || bad "package list lacks $p"
done
