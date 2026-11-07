#!/usr/bin/env bash
# Where the heavy artefacts live (sourced by the build and test scripts).
#
#   SONORA_ARTIFACTS_DIR  one root for the big, sequential files: the ISO and
#                         stick image (iso/), VM disks, overlays, kernels and
#                         serial logs (vm/qemu, vm/usb). Unset: the in-repo
#                         paths, exactly as before (CI and Linux unchanged).
#   SONORA_ISO_DIR, SONORA_QEMU_DIR, SONORA_USB_DIR   override one of them.
#
# This machine's value comes from the untracked .sonora-local.env (KEY=VALUE
# lines, see .sonora-local.env.example), found in the checkout, else in the
# main checkout when this is a worktree. The environment wins over the file.
#
# Only plain sequential files belong there (exFAT: no symlinks, no ownership,
# no permission bits, case-insensitive). The live-build work tree needs a real
# Linux filesystem and stays in a Docker volume (SONORA_LB_VOLUME).
# Run directly (tools/lib/artifacts.sh) it prints the resolved paths.

_art_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

_art_unix() {
  case "$1" in
    [A-Za-z]:*)
      [[ "${1:2:1}" == "\\" || "${1:2:1}" == "/" ]] || { printf "%s" "$1"; return; }
      if command -v cygpath >/dev/null 2>&1; then cygpath -u "$1"; return; fi
      local d="${1:0:1}" rest="${1:2}"
      printf '/%s%s' "$(printf '%s' "$d" | tr 'A-Z' 'a-z')" "${rest//\\//}" ;;
    *) printf '%s' "$1" ;;
  esac
}

_art_load_env() {
  local f="${SONORA_LOCAL_ENV:-}" common key val line
  if [[ -z "$f" ]]; then
    f="$_art_root/.sonora-local.env"
    if [[ ! -f "$f" ]] && common="$(git -C "$_art_root" rev-parse --git-common-dir 2>/dev/null)"; then
      f="$(cd "$_art_root" && cd "$common/.." && pwd)/.sonora-local.env"
    fi
  fi
  [[ -f "$f" ]] || return 0
  while IFS= read -r line || [[ -n "$line" ]]; do
    line="${line%$'\r'}"
    case "$line" in ''|'#'*) continue ;; esac
    key="${line%%=*}"; val="${line#*=}"
    val="${val%\"}"; val="${val#\"}"
    case "$key" in
      SONORA_ARTIFACTS_DIR|SONORA_ISO_DIR|SONORA_QEMU_DIR|SONORA_USB_DIR|SONORA_LB_VOLUME)
        [[ -n "${!key:-}" ]] || export "$key=$val" ;;
    esac
  done <"$f"
}
_art_load_env

if [[ -n "${SONORA_ARTIFACTS_DIR:-}" ]]; then
  SONORA_ARTIFACTS_DIR="$(_art_unix "$SONORA_ARTIFACTS_DIR")"
  SONORA_ISO_DIR="${SONORA_ISO_DIR:-$SONORA_ARTIFACTS_DIR/iso}"
  SONORA_QEMU_DIR="${SONORA_QEMU_DIR:-$SONORA_ARTIFACTS_DIR/vm/qemu}"
  SONORA_USB_DIR="${SONORA_USB_DIR:-$SONORA_ARTIFACTS_DIR/vm/usb}"
fi
SONORA_ISO_DIR="$(_art_unix "${SONORA_ISO_DIR:-$_art_root/iso}")"
SONORA_QEMU_DIR="$(_art_unix "${SONORA_QEMU_DIR:-$_art_root/build-qemu}")"
SONORA_USB_DIR="$(_art_unix "${SONORA_USB_DIR:-$_art_root/build-usb}")"
export SONORA_ISO_DIR SONORA_QEMU_DIR SONORA_USB_DIR

if [[ "${BASH_SOURCE[0]}" == "$0" ]]; then
  echo "SONORA_ISO_DIR=$SONORA_ISO_DIR"
  echo "SONORA_QEMU_DIR=$SONORA_QEMU_DIR"
  echo "SONORA_USB_DIR=$SONORA_USB_DIR"
  echo "SONORA_LB_VOLUME=${SONORA_LB_VOLUME:-sonora-livebuild-work} (docker volume, stays off exFAT)"
fi
