#!/bin/sh
# Run before opening a PR: the cheap things CI would reject, caught locally.
#
#   tools/pre-pr.sh            checks the branch against origin/main
#
# Exit 1 with every problem listed. It builds nothing and runs no tests; the
# tests your change touches are still yours to run (CLAUDE.md "Before opening a
# PR"). Set PRE_PR_ALLOW_PROTOCOL=1 only when the coordinator reserved the next
# kProtocolVersion for you.
set -u

root=$(git rev-parse --show-toplevel) || exit 2
cd "$root" || exit 2
base=${PRE_PR_BASE:-origin/main}
git fetch -q origin 2>/dev/null || echo "pre-pr: warning: could not fetch origin" >&2

fail=0
bad() {
  echo "pre-pr: FAIL: $1" >&2
  fail=1
}

changed=$(git diff --name-only --diff-filter=ACMR "$base"...HEAD)
[ -n "$changed" ] || { echo "pre-pr: no changes against $base"; exit 0; }

# 1. Whitespace errors and conflict markers.
git diff --check "$base"...HEAD >/dev/null 2>&1 || bad "git diff --check reports whitespace errors or conflict markers"

# 2. Executable bits: tools/*.sh, hooks and usr/local/bin must be 100755.
echo "$changed" | while IFS= read -r f; do
  case "$f" in
    tools/*.sh | */usr/local/bin/* | iso/config/hooks/*)
      mode=$(git ls-files -s -- "$f" | cut -c1-6)
      [ "$mode" = 100755 ] || echo "pre-pr: FAIL: $f is mode ${mode:-missing}, want 100755 (git update-index --chmod=+x)" >&2
      ;;
  esac
done | grep -q . && fail=1

# 3. shellcheck on the shell scripts you changed, as CI runs it.
scripts=$(echo "$changed" | grep -E '^tools/.*\.sh$|/usr/local/bin/|\.hook\.chroot$|^iso/auto/config$' || true)
if [ -n "$scripts" ]; then
  # CI checks every tools/*.sh at once, so a sourced helper is an input there;
  # checking only the changed file reports SC1091. Use CI's file list.
  b=iso/config/includes.chroot/usr/local/bin
  ci_scripts=$(ls tools/*.sh iso/auto/config iso/config/hooks/normal/*.hook.chroot \
    "$b/sonora-smoketest" "$b/sonora-display-gave-up" "$b/sonora-install" \
    "$b/sonora-install-tty" "$b/sonora-update" "$b/sonora-policycheck" \
    iso/config/includes.chroot/usr/local/lib/sonora/slots.sh 2>/dev/null)
  if command -v shellcheck >/dev/null 2>&1; then
    # shellcheck disable=SC2086
    shellcheck $ci_scripts || bad "shellcheck"
  elif command -v docker >/dev/null 2>&1; then
    # shellcheck disable=SC2086
    MSYS_NO_PATHCONV=1 docker run --rm -v "$root:/mnt" -w /mnt koalaman/shellcheck:v0.11.0 $ci_scripts || bad "shellcheck (docker)"
  else
    echo "pre-pr: warning: no shellcheck or docker; shellcheck NOT run" >&2
  fi
fi

# 4. ADR numbers: a new ADR must not reuse a number on main or in another open PR.
new_adrs=$(git diff --name-only --diff-filter=A "$base"...HEAD -- 'brain/decisions/ADR-*.md')
for f in $new_adrs; do
  n=$(basename "$f" | sed -n 's/^ADR-\([0-9]*\)-.*/\1/p')
  [ -n "$n" ] || continue
  if git ls-tree -r --name-only "$base" -- brain/decisions | grep -q "ADR-$n-"; then
    bad "ADR-$n already exists on $base"
  fi
  if command -v gh >/dev/null 2>&1; then
    me=$(git rev-parse --abbrev-ref HEAD)
    for pr in $(gh pr list --state open --limit 80 --json number,headRefName --jq ".[] | select(.headRefName != \"$me\") | .number" 2>/dev/null); do
      if gh pr diff "$pr" --name-only 2>/dev/null | grep -q "brain/decisions/ADR-$n-"; then
        bad "ADR-$n is also added by open PR #$pr"
      fi
    done
  fi
done

# 5. Files only the coordinator edits.
echo "$changed" | grep -qE '^planning/(BACKLOG\.md|progress\.json)$' &&
  bad "planning/BACKLOG.md or progress.json edited; put the [x] line and follow-ups in the PR description"

# 6. kProtocolVersion bumps are reserved.
if git diff "$base"...HEAD -U0 | grep -qE '^[+-].*kProtocolVersion *='; then
  [ "${PRE_PR_ALLOW_PROTOCOL:-0}" = 1 ] || bad "kProtocolVersion changed; the coordinator reserves the next version (PRE_PR_ALLOW_PROTOCOL=1 if it did)"
fi

# 7. Unfinished markers.
git diff "$base"...HEAD -U0 | grep -qE '^\+(<{7}|>{7})( |$)' && bad "conflict markers in the diff"

# 8. Big source files (owner, 2026-11-06): a guideline, not a failure. A file
# over 500 lines that this branch grows should be split in the same PR
# (2026-11-06: two 3k-line model files stalled the ASan build for 40 min).
echo "$changed" | grep -E '\.(cpp|cc|h|hpp|py|qml|sh)$' | grep -v '/third_party/' |
  while IFS= read -r f; do
    [ -f "$f" ] || continue
    n=$(wc -l <"$f")
    added=$(git diff --numstat "$base"...HEAD -- "$f" | cut -f1)
    if [ "$n" -gt 500 ] && [ "${added:-0}" -gt 0 ]; then
      echo "pre-pr: warning: $f has $n lines (+$added here); split it by domain (guideline: under 500)" >&2
    fi
  done

# 9. Workflows must parse (a broken ci.yml makes GitHub run zero jobs), and no
# added line may end inside an open quote with the next one starting with it
# (a backslash-n turned into a real line break).
py=$(command -v python3 || command -v python || true)
if [ -n "$py" ]; then
  if ! "$py" -c 'import yaml' 2>/dev/null; then
    echo "pre-pr: warning: PyYAML missing; workflow YAML NOT parsed (pip install pyyaml)" >&2
  else
    # shellcheck disable=SC2046
    "$py" tools/check_syntax.py yaml $(ls .github/workflows/*.yml .github/workflows/*.yaml 2>/dev/null) || bad "workflow YAML does not parse"
  fi
  "$py" tools/check_syntax.py quotes --base "$base" || bad "broken string literal in an added line"
else
  echo "pre-pr: warning: no python; workflow YAML and quote checks NOT run" >&2
fi

if [ "$fail" = 0 ]; then
  echo "pre-pr: ok ($(echo "$changed" | wc -l | tr -d ' ') files). Tests you touched are still yours to run."
fi
exit "$fail"
