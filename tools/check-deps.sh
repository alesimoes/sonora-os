#!/usr/bin/env bash
# Module dependency check (ADR-0007 §1, migration step M5).
#
# The compiler already refuses an include across a CMake edge that is not
# linked. This catches what it cannot: a header that two modules happen to
# both see, a target linked "just for now", and the vendor headers that only
# one directory may include (<jack/>, Qt, plugin SDKs).
#
#   tools/check-deps.sh            check; exit 1 and list every violation
#   tools/check-deps.sh --edges    print the include edges found (for the
#                                  allowlist and for reading the graph)
#   tools/check-deps.sh --test     self-test against a throw-away tree
#
# Every edge between modules must be in tools/module-deps.allow or a
# tools/module-deps.d/*.allow ("from -> to", the ADR §1 graph as built) or
# in tools/module-deps.known ("from -> to", a violation that exists on main
# with a BACKLOG follow-up). A known entry that no longer occurs is an error
# too, so the file shrinks and never rots.
set -euo pipefail

SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="${SONORA_DEPS_ROOT:-$(cd "$SELF/.." && pwd)}"
ALLOW="${SONORA_DEPS_ALLOW:-$SELF/module-deps.allow}"
KNOWN="${SONORA_DEPS_KNOWN:-$SELF/module-deps.known}"
# Drop-in allow files, same format: a feature adds its edges in its own file
# while module-deps.allow is held by another one.
ALLOW_D="${SONORA_DEPS_ALLOW_D:-$SELF/module-deps.d}"
TOPS=(common contracts engine drivers ui plugins interchange assistant)

# A module is its first two path components; plugins/native/* is one group
# because its processors share an SDK by design.
AWK_MODOF='function modof(p,   a) {
    if (p ~ /^plugins\/native\//) return "plugins/native"
    if (p ~ /^interchange(\/|$)/) return "interchange"
    if (p ~ /^assistant(\/|$)/) return "assistant"
    split(p, a, "/"); return a[1] "/" a[2]
}'

# Prints "from -> to" per include that crosses a module, and
# "vendor: ..." for a forbidden vendor include. Two processes in all: a
# process per file is minutes on Windows.
scan() {
    cd "$ROOT"
    local tops=() d
    for d in "${TOPS[@]}"; do [ -d "$d" ] && tops+=("$d"); done
    [ "${#tops[@]}" -gt 0 ] || return 0
    {
        find "${tops[@]}" -path '*/include/*' -type f | sed 's/^/H\t/'
        grep -rHoE --include='*.h' --include='*.hpp' --include='*.cpp' --include='*.cc' \
            '^[[:space:]]*#[[:space:]]*include[[:space:]]*[<"][^>"]+' "${tops[@]}" |
            sed -E 's/^([^:]+):[[:space:]]*#[[:space:]]*include[[:space:]]*[<"]/I\t\1\t/' || true
    } | awk -F'\t' "$AWK_MODOF"'
        $1 == "H" {
            p = $2; i = index(p, "/include/")
            owner[substr(p, i + 9)] = modof(substr(p, 1, i - 1)); next
        }
        $1 == "I" {
            f = $2; h = $3; m = modof(f)
            if (h ~ /^jack\// && m != "drivers/jack")
                print "vendor: " f " includes <" h "> (only drivers/jack may)"
            else if (h ~ /^Q[A-Z]/ && m != "ui/qt")
                print "vendor: " f " includes <" h "> (only ui/qt may)"
            else if (h ~ /^(lilv|lv2|vst3sdk|pluginterfaces|aaf|libaaf|ptformat)\// \
                     && m != "plugins/host" && m != "plugins/native" && m !~ /^interchange/)
                print "vendor: " f " includes <" h "> (plugin SDKs and interchange libraries stay in plugins/host and interchange)"
            if ((h in owner) && owner[h] != m) print m " -> " owner[h]
        }'
}

list() { grep -vE '^[[:space:]]*(#|$)' "$1" 2>/dev/null | sed -E 's/[[:space:]]+#.*$//; s/[[:space:]]+$//' | sort -u || true; }

check() {
    local found vendor edges allow known bad=0 line
    found="$(scan | sort -u)"
    vendor="$(printf '%s\n' "$found" | grep '^vendor:' || true)"
    edges="$(printf '%s\n' "$found" | grep -- ' -> ' || true)"
    allow="$(list "$ALLOW"; for f in "$ALLOW_D"/*.allow; do if [ -f "$f" ]; then list "$f"; fi; done)"
    known="$(list "$KNOWN")"

    if [ -n "$vendor" ]; then
        printf 'forbidden vendor include:\n%s\n' "$vendor"
        bad=1
    fi
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        if printf '%s\n' "$allow" | grep -qxF -- "$line"; then continue; fi
        if printf '%s\n' "$known" | grep -qxF -- "$line"; then continue; fi
        echo "forbidden edge (ADR-0007 §1): $line"
        bad=1
    done <<< "$edges"
    while IFS= read -r line; do
        [ -n "$line" ] || continue
        if ! printf '%s\n' "$edges" | grep -qxF -- "$line"; then
            echo "stale entry in $(basename "$KNOWN"), delete it: $line"
            bad=1
        fi
    done <<< "$known"
    if [ "$bad" -eq 0 ]; then
        echo "module dependencies: ok ($(printf '%s\n' "$edges" | grep -c . || true) edges)"
    fi
    return "$bad"
}

selftest() {
    local t
    t="$(mktemp -d)"
    trap 'rm -rf "$t"' RETURN
    mkdir -p "$t/ui/keymap/src" "$t/engine/core/include/sonora" "$t/common/base/include/sonora" "$t/contracts/api/include/sonora"
    : > "$t/engine/core/include/sonora/session.h"
    : > "$t/common/base/include/sonora/ring.h"
    : > "$t/contracts/api/include/sonora/api.h"
    : > "$t/known"
    printf '#include "sonora/api.h"\n#include "sonora/ring.h"\n' > "$t/ui/keymap/src/a.cpp"
    printf 'ui/keymap -> contracts/api\nui/keymap -> common/base\n' > "$t/allow"
    mkdir -p "$t/allow.d"
    ROOT="$t" ALLOW="$t/allow" KNOWN="$t/known" ALLOW_D="$t/allow.d"
    check > /dev/null || { echo "selftest: clean tree rejected"; return 1; }
    printf '#include "sonora/api.h"\n' > "$t/engine/core/src.cpp"
    if check > /dev/null; then echo "selftest: unlisted edge accepted"; return 1; fi
    printf 'engine/core -> contracts/api\n' > "$t/allow.d/x.allow"
    check > /dev/null || { echo "selftest: drop-in allow edge rejected"; return 1; }
    printf '#include "sonora/session.h"\n' >> "$t/ui/keymap/src/a.cpp"
    if check > /dev/null; then echo "selftest: ui -> engine accepted"; return 1; fi
    printf 'ui/keymap -> engine/core\n' > "$t/known"
    check > /dev/null || { echo "selftest: known edge rejected"; return 1; }
    printf '#include <jack/jack.h>\n' >> "$t/ui/keymap/src/a.cpp"
    if check > /dev/null; then echo "selftest: jack outside drivers/jack accepted"; return 1; fi
    # assistant/ is one module (ADR-0155) and must be scanned: an edge to
    # engine/* is refused, not silently unseen.
    : > "$t/known"
    printf '#include "sonora/api.h"\n#include "sonora/ring.h"\n' > "$t/ui/keymap/src/a.cpp"
    mkdir -p "$t/assistant/src/core"
    printf '#include "sonora/ring.h"\n' > "$t/assistant/src/core/a.cpp"
    printf 'assistant -> common/base\n' >> "$t/allow.d/x.allow"
    check > /dev/null || { echo "selftest: allowed assistant edge rejected"; return 1; }
    printf '#include "sonora/session.h"\n' >> "$t/assistant/src/core/a.cpp"
    if check > /dev/null; then echo "selftest: assistant -> engine accepted"; return 1; fi
    echo "check-deps selftest: ok"
}

case "${1:-}" in
    --edges) scan | sort -u ;;
    --test) selftest ;;
    "") check ;;
    *) echo "usage: $0 [--edges|--test]" >&2; exit 2 ;;
esac
