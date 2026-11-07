#!/usr/bin/env bash
# The plugin package (owner, 2026-11-05: "vamos disponibilizar um pacote de
# plugin"): the native suite's LV2 and VST3 bundles with the licence texts and
# a README, as dist/sonora-plugins-<version>-linux-x86_64.tar.gz. The Windows
# VST3 zip is tools/package-plugins.ps1.
#
#   tools/package-plugins.sh              build (plugins-only tree) + package
#   tools/package-plugins.sh --no-build   package the stage a previous run left
#   tools/package-plugins.sh --out DIR    write the archive under DIR (default dist/)
#
# What goes in is what the install rules of plugins/native/format install, so a
# processor marked EXPERIMENTAL, or one that has not met its CPU class
# (ADR-0011 3.5), is not packaged. Third-party notices are the ones
# brain/research/plugin-licences-onnx.md lists: DPF's ISC text must travel with
# any binary containing DPF code.
#
# The version is SONORA_VERSION if set, else git describe; a tree with no tag
# reads 0.0.0-<commits>-g<sha>.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"

BUILD=1
OUT="$ROOT/dist"
while [[ $# -gt 0 ]]; do
  case "$1" in
    --no-build) BUILD=0 ;;
    --out) OUT="$2"; shift ;;
    *) echo "usage: $0 [--no-build] [--out DIR]" >&2; exit 2 ;;
  esac
  shift
done

version() {
  if [[ -n "${SONORA_VERSION:-}" ]]; then printf '%s' "$SONORA_VERSION"; return; fi
  local d
  d="$(git describe --tags --always 2>/dev/null || echo unknown)"
  if [[ "$d" =~ ^v?[0-9] ]]; then
    printf '%s' "${d#v}"
  else
    printf '0.0.0-%s-g%s' "$(git rev-list --count HEAD 2>/dev/null || echo 0)" "$d"
  fi
}
VER="$(version)"
NAME="sonora-plugins-${VER}-linux-x86_64"

if ((BUILD)); then
  tools/build-plugins.sh --plugins-only --stage
fi

STAGE="$ROOT/build-plugins-only/stage/usr/local/lib"
[[ -d "$STAGE/lv2" && -d "$STAGE/vst3" ]] || {
  echo "error: no staged bundles under $STAGE (run without --no-build)" >&2
  exit 1
}

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
PKG="$WORK/$NAME"
mkdir -p "$PKG/lv2" "$PKG/vst3" "$PKG/licenses"
cp -a "$STAGE"/lv2/. "$PKG/lv2/"
cp -a "$STAGE"/vst3/. "$PKG/vst3/"
cp LICENSE "$PKG/licenses/Sonora-GPL-3.0.txt"
cp plugins/native/format/third_party/dpf/LICENSE "$PKG/licenses/DPF-ISC.txt"
cp plugins/native/format/third_party/dpf/LICENSING.md "$PKG/licenses/DPF-LICENSING.md"
cat >"$PKG/licenses/NOTICE.txt" <<N
Sonora plugins $VER

The plugins are licensed under the GNU General Public License, version 3
(Sonora-GPL-3.0.txt). They are built on DPF, the DISTRHO Plugin Framework,
Copyright (C) 2012-2025 Filipe Coelho, under the ISC licence (DPF-ISC.txt);
its LV2 and VST3 parts are ISC, and no part of Steinberg's VST3 SDK is used.
No sample, impulse response, preset or model of another product is included.
N
nl=$(find "$PKG/lv2" -maxdepth 1 -name '*.lv2' | wc -l)
nv=$(find "$PKG/vst3" -maxdepth 1 -name '*.vst3' | wc -l)
cat >"$PKG/README.txt" <<R
Sonora plugins $VER (Linux x86_64)

$nl LV2 bundles in lv2/, $nv VST3 bundles in vst3/. Each processor comes in a
stereo build and a "-mono" build (one input, one output).

Install for one user:
  mkdir -p ~/.lv2 ~/.vst3
  cp -r lv2/*  ~/.lv2/
  cp -r vst3/* ~/.vst3/
or system wide into /usr/local/lib/lv2 and /usr/local/lib/vst3.

Any LV2 or VST3 host loads them; Sonora loads them out of process.
Licences and notices: licenses/.
R

mkdir -p "$OUT"
ARCHIVE="$OUT/$NAME.tar.gz"
tar --owner=0 --group=0 --numeric-owner --sort=name -czf "$ARCHIVE" -C "$WORK" "$NAME"
echo "package: $ARCHIVE ($nl LV2, $nv VST3)"
