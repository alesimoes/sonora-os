"""Paths under .claude that agent runs may write (owner, 2026-11-08).

Claude Code treats .claude/ as sensitive and refuses Edit/Write there even in
acceptEdits mode, which blocked plugins.stock.docs and
system.hardening.ro_root.adr (their job is to edit .claude/context). A rule
naming the exact path overrides that refusal; nothing else under .claude
(agents, settings, hooks, skills, routing) is opened. The leading "/" makes
the pattern relative to the run's project root (its worktree).
"""

WRITABLE_CLAUDE_DIRS = (".claude/context", ".claude/rules")
EDIT_TOOLS = ("Edit", "Write", "MultiEdit")


def writable_rules():
    return [f"{tool}(/{d}/**)" for d in WRITABLE_CLAUDE_DIRS
            for tool in EDIT_TOOLS]
