#!/usr/bin/env bash
# Prints where the work stands, for Claude Code's SessionStart hook
# (.claude/settings.json). Whatever this prints is added to the new session's
# context, so it is the whole of what a session reads before it starts
# working: a few dozen lines instead of a transcript or the planning files.
#
# It must be cheap and must never fail a session start: no network, no
# Docker, every step allowed to come up empty.

cd "${CLAUDE_PROJECT_DIR:-$(dirname "${BASH_SOURCE[0]}")/..}" 2>/dev/null || exit 0

MAX_HANDOFF_LINES=45

branch="$(git branch --show-current 2>/dev/null)"
dirty="$(git status --porcelain 2>/dev/null | wc -l | tr -d ' ')"

echo "## Where we left off (from tools/session-start.sh)"
echo
echo "Branch \`${branch:-detached}\`, ${dirty} uncommitted path(s). Last commits:"
git log --oneline -3 2>/dev/null | sed 's/^/    /'
echo

# The main session's own handoff: what is in flight across branches.
if [[ -f planning/handoffs/main-session.md ]]; then
  echo "### planning/handoffs/main-session.md"
  head -n "$MAX_HANDOFF_LINES" planning/handoffs/main-session.md
  echo
fi

# This branch's handoff, if it has one.
mine="planning/handoffs/${branch//\//-}.md"
if [[ -n "$branch" && -f "$mine" ]]; then
  echo "### $mine"
  head -n "$MAX_HANDOFF_LINES" "$mine"
  echo
fi

# Every other open handoff, as one line each.
others=0
for f in planning/handoffs/*.md; do
  [[ -f "$f" ]] || continue
  case "$f" in
    */README.md|*/main-session.md|"$mine") continue ;;
  esac
  if [[ "$others" == 0 ]]; then echo "Other open handoffs:"; fi
  others=$((others + 1))
  state="$(grep -m1 -- '- \*\*State:\*\*' "$f" | sed 's/.*\*\*State:\*\* *//')"
  echo "- $f: ${state:-no State line}"
done
[[ "$others" == 0 ]] || echo

if [[ -f planning/STATUS.md ]]; then
  grep -m1 -- '- \*\*Updated:\*\*' planning/STATUS.md
  # The owner's pending decision, if any, is the one thing never to miss.
  awk '/^## Next decision needed/{p=1; next} /^## /{p=0} p && NF' planning/STATUS.md \
    | head -n 6 | sed 's/^/Owner decision pending: /;2,$s/^Owner decision pending: /    /'
fi

echo
echo "Read more only as the task needs it: the handoff's Pointers, then"
echo "planning/STATUS.md. Stopping or switching task: update the handoff first"
echo "(handoff skill)."
exit 0
