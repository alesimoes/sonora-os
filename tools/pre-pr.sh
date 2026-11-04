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
  if command -v shellcheck >/dev/null 2>&1; then
    # shellcheck disable=SC2086
    shellcheck $scripts || bad "shellcheck"
  elif command -v docker >/dev/null 2>&1; then
    # shellcheck disable=SC2086
    MSYS_NO_PATHCONV=1 docker run --rm -v "$root:/mnt" -w /mnt koalaman/shellcheck:stable $scripts || bad "shellcheck (docker)"
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

if [ "$fail" = 0 ]; then
  echo "pre-pr: ok ($(echo "$changed" | wc -l | tr -d ' ') files). Tests you touched are still yours to run."
fi
exit "$fail"
