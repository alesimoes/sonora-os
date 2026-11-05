#!/usr/bin/env bash
# contracts/CONTRACTS.lock (ADR-0007 §6): a hash of every contract file, so a
# change to one is deliberate. The contract files are the public headers
# under contracts/*/include and common/model/include.
#
#   tools/contracts-lock.sh           fail when a contract file changed and
#                                     the lock was not regenerated
#   tools/contracts-lock.sh --update  rewrite the lock
#   tools/contracts-lock.sh --test    self-test
#
# Regenerating the lock is the "I know this is a contract change" step: the
# PR then needs the §6 reviews of every owning party, and an ADR amendment
# when the change is breaking. If the wire or shared-memory layout moved, the
# version constant moves in the same PR.
set -euo pipefail

SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="${SONORA_LOCK_ROOT:-$(cd "$SELF/.." && pwd)}"
LOCK="$ROOT/contracts/CONTRACTS.lock"

# Line endings are normalised so a Windows checkout hashes like Linux.
render() {
    cd "$ROOT"
    local f
    echo "# Hash of every contract file (ADR-0007 §6). Regenerate with"
    echo "# tools/contracts-lock.sh --update; see the header of that script."
    find contracts common/model/include -path '*/include/*' -type f 2>/dev/null | LC_ALL=C sort |
        while IFS= read -r f; do
            printf '%s  %s\n' "$(tr -d '\r' < "$f" | sha256sum | cut -d' ' -f1)" "$f"
        done
}

check() {
    local have
    have="$(render)"
    if [ ! -f "$LOCK" ]; then
        echo "contracts/CONTRACTS.lock is missing; run tools/contracts-lock.sh --update" >&2
        return 1
    fi
    if ! diff <(tr -d '\r' < "$LOCK") <(printf '%s\n' "$have") > /dev/null; then
        echo "A contract file changed and contracts/CONTRACTS.lock was not regenerated." >&2
        diff <(tr -d '\r' < "$LOCK") <(printf '%s\n' "$have") | grep '^[<>]' >&2 || true
        echo "If the change is deliberate: tools/contracts-lock.sh --update, and see ADR-0007 §6 for the reviews it needs." >&2
        return 1
    fi
    echo "contracts lock: ok ($(printf '%s\n' "$have" | grep -vc '^#') files)"
}

selftest() {
    local t
    t="$(mktemp -d)"
    trap 'rm -rf "$t"' RETURN
    mkdir -p "$t/contracts/api/include/sonora" "$t/common/model/include/sonora"
    printf 'v1\n' > "$t/contracts/api/include/sonora/a.h"
    printf 'm1\n' > "$t/common/model/include/sonora/m.h"
    ROOT="$t" LOCK="$t/contracts/CONTRACTS.lock"
    check > /dev/null 2>&1 && { echo "selftest: missing lock accepted"; return 1; }
    render > "$LOCK"
    check > /dev/null || { echo "selftest: fresh lock rejected"; return 1; }
    printf 'v2\n' > "$t/contracts/api/include/sonora/a.h"
    check > /dev/null 2>&1 && { echo "selftest: changed header accepted"; return 1; }
    render > "$LOCK"
    : > "$t/contracts/api/include/sonora/new.h"
    check > /dev/null 2>&1 && { echo "selftest: new header accepted"; return 1; }
    echo "contracts-lock selftest: ok"
}

case "${1:-}" in
    --update) render > "$LOCK"; echo "wrote $LOCK" ;;
    --test) selftest ;;
    "") check ;;
    *) echo "usage: $0 [--update|--test]" >&2; exit 2 ;;
esac
