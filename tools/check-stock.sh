#!/usr/bin/env bash
# check-stock.sh [MANIFEST LIST]: the stock effects set (ADR-0076) is what the
# image installs. Fails when the manifest and the package list differ, a spdx
# is off the manifest's allow-list, a category is outside the six, or a name
# matches the brand deny-list (ADR-0011 section 5: no third-party product,
# company or artist name).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MAN="${1:-$ROOT/plugins/content/stock.json}"
LIST="${2:-$ROOT/iso/config/package-lists/sonora-stock.list.chroot}"
CATS="eq dynamics delay reverb modulation saturation"
# Word-bounded, case-insensitive. Extend with a manifest PR, never silently.
DENY='apple|avid|pro[ _-]?tools|logic|waves|universal audio|neve|ssl|fairchild|pultec|urei|lexicon|eventide|hendrix|clapton'
fail=0
bad() { echo "FAIL: $*" >&2; fail=1; }
[ -f "$MAN" ] || { echo "FAIL: no manifest $MAN" >&2; exit 1; }
[ -f "$LIST" ] || { echo "FAIL: no list $LIST" >&2; exit 1; }

allow="$(sed -n '/"spdx_allow"/,/\]/p' "$MAN" | grep -o '"[A-Za-z0-9.+-]*"' | tr -d '"' | grep -v '^spdx_allow$' || true)"
pkgs="$(grep -o '"package": *"[^"]*"' "$MAN" | sed 's/.*: *"\(.*\)"/\1/' | sort)"
listed="$(grep -vE '^[[:space:]]*(#|$)' "$LIST" | tr -d ' \r' | sort)"
[ -n "$pkgs" ] || bad "manifest has no packages"
[ -n "$allow" ] || bad "manifest has no spdx_allow"

while read -r p; do
  [ -n "$p" ] || continue
  printf '%s\n' "$listed" | grep -qx -- "$p" || bad "manifest package $p is missing from the list"
done <<< "$pkgs"
while read -r p; do
  [ -n "$p" ] || continue
  printf '%s\n' "$pkgs" | grep -qx -- "$p" || bad "list package $p is not in the manifest"
done <<< "$listed"

# Each spdx identifier of each entry must be allowed; "AND"/"OR" only join them.
while read -r expr; do
  [ -n "$expr" ] || continue
  for id in $(printf '%s\n' "$expr" | sed -E 's/ (AND|OR) / /g; s/[()]//g'); do
    printf '%s\n' "$allow" | grep -qx -- "$id" || bad "spdx $id is not on the allow-list"
  done
done <<< "$(grep -o '"spdx": *"[^"]*"' "$MAN" | sed 's/.*: *"\(.*\)"/\1/')"

while read -r line; do
  for c in $(printf '%s\n' "$line" | grep -o '"[a-z]*"' | tr -d '"'); do
    case " $CATS " in *" $c "*) ;; *) bad "category $c is outside: $CATS" ;; esac
  done
done <<< "$(grep '"category"' "$MAN" | sed 's/.*"category"://')"

if grep -Eiq "(^|[^a-z])($DENY)([^a-z]|\$)" <<< "$(grep -E '"(package|replaced_by|retire_when)"' "$MAN")
$listed"; then
  bad "a name matches the brand deny-list (ADR-0011 section 5)"
fi
[ "$fail" -eq 0 ] && echo "ok: $(printf '%s\n' "$pkgs" | grep -c .) stock packages, list and manifest agree"
exit "$fail"
