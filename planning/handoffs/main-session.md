# main session

The coordinating session's handoff. Overwrite it at every stopping point.

- **State:** 2026-10-25, late. Running unattended under `/loop` (dynamic,
  30 min fallback). Owner: keep the cap at 3 agents (cost); /clear can't be
  run by the model — rely on auto-compaction + this file.

## Verify
    git fetch origin && git log --oneline -8 origin/main
    gh pr list --state open

## Done this session (all on main)
- #123 transport.locate; #120 M-1c part 2 (protocol 10); #121 plugin touches
  lost/unmapped; #125 Windows VST3 via yabridge (ADR-0017 accepted);
  #127 M-1c part 3, engine saver owns MIDI Sequences/ (protocol 11, ADR-0015
  and ADR-0007 updates accepted); #126 loop playback when not recording.
- Merge: run origin/main's tools/merge-when-green.sh, copied to the
  scratchpad (main checkout is an old detached HEAD, b487313). Queue PRs
  one after another. On a GitHub network failure, wait for `gh api
  rate_limit` and retry.
- The repo has no approval labels; the verdict is the PR comment.

## In flight (2026-10-26)
- UNATTENDED: no human interacts. Never stop to wait for /clear or answers
  (CLAUDE.md rule via PR #145). Keep launching the next items.
- Main: protocol 17, session format 9. Merged this run: #132 #135 #136
  #137 #138 #139 #140 #141 #142 #143 #144.
- Also merged: #145 harness (unattended rule), #146 plugin/DSP test
  follow-ups, #147 ADR-0018 step 3 (orphans per folder, saver fence).
- PAUSED 2026-10-26 for a laptop restart. Everything is pushed. Resume with:
  - PR #149, transport.toggle_loop_record + #144 follow-ups (format 10,
    protocol 18). CI was running at the pause. Its branch handoff
    planning/handoffs/t1-toggle-loop-record.md must be deleted before the
    merge. Retake its screenshots (the Logic "Cycle" caption). Review: Opus
    (storage + contract).
  - PR #150, ADR-0018 step 4 untitled sessions + #142 N2 + #147 N-A/N-B
    (protocol 20). CI is RED: the plugin host job (likely
    plugin_hosting_flush_read_across_load timing) and RTSan. Read
    `gh run view 40333459943 --log-failed` and fix. Branch handoff
    planning/handoffs/t1-untitled-sessions.md (delete before the merge).
  - Branch t1-latency-wire, ADR-0019 follow-ups (protocol 21). WIP, NO PR,
    never built. Branch handoff planning/handoffs/t1-latency-wire.md has
    the ordered Open list. Known fix: test_wire.cpp:360 expects version 17.
  - Protocol numbers will need renumbering in merge order (18, 20, 21).
  - Then a new architect slot: ADR-0021 (below).
- Then ADR-0018 steps 5 (Save As), 6 (Gather), 7 (medium checks), revised by ADR-0021.
- Also merged: #148 track input monitor (no format/protocol change).
  Follow-up: the I button lit-but-inactive when the engine forces Off.
- Running instead of input monitor: ADR-0019 follow-ups, latency over the
  wire + Latency page + per-machine L (t1-latency-wire, protocol 21).
- Follow-ups to batch: #146 wine_refused test doesn't exercise refuse()
  (fake host sets bridge itself); the research note's 400 Hz example.
- Track 1 follow-ups still to batch: #136 loop-L test is a guard only;
  `lat=` downgrade rejection; `~0` playhead sentinel; #142 N2 (first Save
  renames the session after its folder).
- Next items after these: ADR-0018 steps 4-7, track.toggle_input_monitor,
  the latency probe over the wire with a UI page (ADR-0019 follow-ups),
  structured positions protocol, BACKLOG split.
- Merge script: /c/Users/Alexandre/AppData/Local/Temp/claude/C--source-sound/
  160a57f4-93dc-476f-acf3-66fbe997cc7a/scratchpad/mwg.sh (copy of
  origin/main tools/merge-when-green.sh; use the absolute path). If the CI
  RTSan job sits queued with no runner: cancel the run, then `gh run rerun --failed`.
- Docker: owner said leave it. Old worktree agent-a7dc34a0146c7c6e3 can go.
Review: tiered per CLAUDE.md; only blocking findings send a PR back.

## ADR-0018 (accepted 2026-10-25, PR #133): the session folder owns its media
- 7 implementation PRs in its checklist, in order; step 3 removes #132's
  session-name/id filter. Researcher: 4 Unverified measurements (checkpoint
  latency on a real USB stick decides whether take files are preallocated).
- PROTOCOL COLLISION: #132 and #134 both bump 11→12; the second to merge
  renumbers to 13.

## Next in the queue
- engine.playback.input_monitor (M), engine.record.orphan_offer (S),
  engine.core.click (S), transport.toggle_loop (needs a loop-enabled session
  setting: format change, decide on the recommended default), structured
  positions protocol, BACKLOG split; Track 4 yabridge follow-ups (in BACKLOG).
- Non-blocking follow-ups: #123 tests hard-code 2^48 (use kMaxLocateFrame);
  #121 tolerance 1e-6 vs stepped params; #126 N1–N4 (re-read before the first
  wrap after punch-out, a possible edit/loop-change standoff, show() changes
  the loop on failure, test gaps) and unused loop_take_running(); #127
  keep_older_version nests Session File Backups/, 64 MiB Save cap (in
  BACKLOG); #120 Snapshot cancellation, silent in-process overflow, unstarted
  Loads not in the held cap; #128 check_denormals shares `out` between quiet
  and active, so only odd-k quiet blocks are inspected (separate buffer);
  #129 counter: timecode past 24 h refused as "before session start", click
  on margin while editing cycles format, misleading refusals (16+ digit
  samples, `.5` in Min:Secs); BACKLOG: tempo/meter editing on rulers, a key
  that opens counter entry.
- PRIORITY follow-up from #130 (review comment 6401669492, guard + test
  suggested there): a loop range later on the timeline disables post-roll
  from roll start, so play "2000-3000" with loop [100000,200000) runs into
  the loop forever. Also: Qt must check the version or capability before
  sending the "A-B" play range (old engines ignore it); Qt selection → play
  argument; plain take has no pre-roll.
- #131 follow-up: a plugin state deleted after open is saved silently with
  no caution (clean() then runs) — set `caution` + test. Recorded audio has
  the same root cause as #131 B1 (files in the engine --dir, not the session
  folder) — in BACKLOG Track 4 item 5.

## Cleanup (2026-10-25)
- Worktrees 131 → 12 (active only). Every removed HEAD is kept as
  refs/archive/wt/<name> (117 refs), WIP-committed if it was dirty. clean.sh
  reclaimed 4 GB of Docker. Owner: keep planning/ as it is. Repo setting
  delete_branch_on_merge is false; the merge script deletes branches anyway
  (offered to turn it on, no answer yet). Janitor every 2–3 merges.

## Cleanup pending (older notes)
- STATUS.md stale: punch in block (#81…) is done but still listed as "next".
- Remote branches t1-loop-record, t3k-paste-fill (deleting them was blocked
  by auto mode; the owner runs it with `!`).
- Many worktrees (fix120-own with build-tsan/, the agent-* ones) → janitor.
- build-fix120/ missing from .gitignore.

## Rules
- Take recommended decisions; ask the owner only for real blockers.
- Lean briefs; fresh agent per large fix round; reports ≤5 lines.
