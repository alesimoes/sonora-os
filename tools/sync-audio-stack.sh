#!/usr/bin/env bash
# audio-stack/ is the documented source for the tuning files; the image needs
# copies inside config/includes.chroot. This refreshes them. Run it after
# editing anything in audio-stack/.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="$ROOT/iso/config/includes.chroot/usr/share/sonora"

mkdir -p "$DEST"
for f in limits-audio.conf sysctl-sonora.conf pipewire-sonora.conf wireplumber-sonora.conf; do
  cp -v "$ROOT/audio-stack/$f" "$DEST/$f"
done
