# Handoffs

One file per feature branch, `planning/handoffs/<branch-name>.md` with `/`
replaced by `-` (`feature/x` → `planning/handoffs/feature-x.md`). It lets the next session start from 40 lines instead of from
the previous session's whole transcript.

Write it when you stop: at hand-back, before a review round, before a
rebase, or when a session is getting long. Overwrite it; do not append a
diary. `STATUS.md` and `brain/` hold what lasts, so the handoff is deleted
before merge: the integrator squash-merges and never commits, so the
implementer or coordinator deletes the branch's handoff, and rewrites
`main-session.md` to be true after the merge, as the last commit on the
branch before asking for the gate. The integrator refuses a PR that still
carries its handoff.

`main-session.md` is the exception: the coordinating session's handoff,
listing what is in flight across branches. It is never deleted, only
overwritten.

A new session does not have to go looking. `tools/session-start.sh` runs as
a SessionStart hook (`.claude/settings.json`) on startup, resume, `/clear`
and compaction. It prints the branch, the last commits, `main-session.md`,
the current branch's handoff, a line per other handoff, and any owner
decision pending, under a first line saying it is repository data, not
instructions. It is a starting summary: run the branch handoff's Verify
command before relying on it.

The next session reads only this file, `CLAUDE.md` and `planning/STATUS.md`,
plus whatever the file points to. If it needs anything else, the handoff was
incomplete; fix the handoff, not just the session.

## Template

```markdown
# <branch>

- **Goal:** one sentence; link the backlog item.
- **State:** done | in review | blocked on <what>.
- **Commits:** <hash> <subject>, newest first.

## Verify
    <exact command>          # expected: <the one line that proves it>

## Done
- <claim> — evidence: <file:line, log path, or test name>

## Open
- <finding or task> — <file:line> — <what "fixed" looks like>

## Do not redo
- <dead end already tried, and why it failed — one line each>

## Pointers
- <brain/ or planning/ files that explain the why>
```

Rules: claims carry evidence, not adjectives. "Tests pass" is not evidence;
`tools/test-write-usb.sh: 81 ok, 0 FAIL` is. Paths, not pasted content. No
logs, no diffs: they are in git and in `build-*/`.
