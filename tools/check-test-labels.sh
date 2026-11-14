#!/usr/bin/env bash
# Static ctest label check (ADR-0007 section 7).
#
# Every add_subdirectory() in the root CMakeLists.txt whose directory has a
# tests/ folder must have a sonora_label_module_tests(<dir> <label>) line, or
# `ctest -L <module>` silently skips that module's tests.
#
#   tools/check-test-labels.sh          check; exit 1 and list the gaps
#   tools/check-test-labels.sh --test   self-test against a throw-away tree
set -euo pipefail

SELF="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

check() {
    local root="$1" bad=0 dir
    local cml="$root/CMakeLists.txt"
    while read -r dir; do
        [ -d "$root/$dir/tests" ] || continue
        if ! grep -Eq "^[[:space:]]*sonora_label_module_tests\(${dir}[[:space:]]+[A-Za-z_]+\)" "$cml"; then
            echo "check-test-labels: $dir has tests/ but no sonora_label_module_tests line" >&2
            bad=1
        fi
    done < <(sed -nE 's/^[[:space:]]*add_subdirectory\(([A-Za-z0-9_/.-]+)\).*/\1/p' "$cml" | sort -u)
    return "$bad"
}

self_test() {
    local t rc
    t="$(mktemp -d)"
    trap 'rm -rf "$t"' RETURN
    mkdir -p "$t/a/tests" "$t/b/tests" "$t/c"
    printf 'add_subdirectory(a)\n  add_subdirectory(b)\nadd_subdirectory(c)\nsonora_label_module_tests(a x)\n' > "$t/CMakeLists.txt"
    rc=0; check "$t" 2>/dev/null || rc=$?
    [ "$rc" -eq 1 ] || { echo "self-test: missing label for b not caught" >&2; return 1; }
    printf 'sonora_label_module_tests(b y)\n' >> "$t/CMakeLists.txt"
    check "$t" || { echo "self-test: labelled tree rejected" >&2; return 1; }
    echo "check-test-labels: self-test ok"
}

case "${1:-}" in
    --test) self_test ;;
    "") check "$(cd "$SELF/.." && pwd)" && echo "check-test-labels: ok" ;;
    *) echo "usage: $0 [--test]" >&2; exit 2 ;;
esac
