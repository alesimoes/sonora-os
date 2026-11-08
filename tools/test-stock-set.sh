#!/usr/bin/env bash
# check-stock.sh accepts a good set and rejects a GPL-2.0-only entry and a
# stray package: a checker that cannot fail proves nothing.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
CHK="$ROOT/tools/check-stock.sh"
FX="$ROOT/tools/fixtures/stock"
rc=0
bash "$CHK" "$FX/good/stock.json" "$FX/good/list" >/dev/null || { echo "FAIL: the good fixture was rejected" >&2; rc=1; }
for bad in gpl2only stray; do
  if bash "$CHK" "$FX/$bad/stock.json" "$FX/$bad/list" 2>/dev/null; then
    echo "FAIL: the $bad fixture was accepted" >&2; rc=1
  else
    echo "rejected: $bad"
  fi
done
bash "$CHK" >/dev/null || { echo "FAIL: the real manifest and list disagree" >&2; rc=1; }
[ "$rc" -eq 0 ] && echo "ok: stock set checker accepts good, rejects gpl2only and stray"
exit "$rc"
