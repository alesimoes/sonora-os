# 2026-11-06: triage of the work the disk-full stop left uncommitted

The disk filled, the orchestrator daemon died, and many agent worktrees held
uncommitted or unpushed work. Each was snapshotted to
`origin/backup/2026-11-06/<name>`: one WIP commit (tracked and untracked
files, build output excluded) on top of the worktree's HEAD. This note says,
for each, whether the content is on main, and what becomes of it.

## Method

Against `origin/main` at `cd874e0a` (#454) and the open PRs (#446, #455-#464):

- for every file of the WIP commit alone, and of the whole branch from its
  merge base, `git apply -R --check` of that file's patch on main (succeeds:
  the content is on main), else `git apply --check` (applies cleanly: it is
  missing), else a 3-way check (it was touched since);
- blob comparison against the open PRs' heads for the files they share;
- for content that is missing, a search of main for the feature under another
  name (most merges were squashes or bundles, so ancestry says nothing), and
  a read of the diff.

Classes: **ON MAIN** (the content landed, maybe squashed or rewritten),
**SUPERSEDED** (main solved it another way), **OPEN PR**, **MISSING** (valuable
and not anywhere), **NOTHING** (only logs, build output or runtime state).

## Table

| Branch | Class | Evidence | Action |
|---|---|---|---|
| harness-rules-dashboard | MISSING (dashboard, icon); OPEN PR (agents) | `tools/dashboard.{py,html}` of the backup is the live one (avatars, flow board, build-time view, project completion, local-day merged list); main has the older #363/#406 copy. `docs/images/brand/sonora-icon.svg` (owner's choice) absent. Agents: lead, principal-engineer, tester identical on main; flake-fixer, refactor-engineer, release-engineer, visual-designer byte-identical in #458; harness-engineer identical in #456. CLAUDE.md, pre-pr.sh, merge-when-green, session-start, dashboard-up: main is newer | PR with the dashboard and icon (this note's PR); agents ride #456/#458 |
| faceplates-mockup | MISSING (design input) | `docs/images/plugins/mockups/` (SN-Lume, SN-Talon, SN-Aurea, round 3 plus an uncommitted simplification) not on main or in any PR; the README says "images only, for the owner to approve the look before any editor code is written" | Kept on backup; ships with the faceplate editors once the owner approves the look (no standalone docs PR, CLAUDE.md "no PR for half an item") |
| native-editors | MISSING, partial | Telemetry contract landed (#427 via #436); the two later commits (engine read loop, descriptor hints, InsertPanel sections/meters/curves, A/B, 72 files) are on no branch of main and #427's follow-up was never opened | Kept on backup; needs a rebase over 35 commits, the 2026-11-06 faceplate direction (it draws a generic panel), screenshots, review |
| fix-chip | ON MAIN | "+N hidden-plugin chip" code reverse-applies on main (`ChannelStrip.qml`, `test_inserts.cpp`); mix views are #175 | None |
| edit-core-shuffle-spot | ON MAIN (committed part); MISSING, partial (WIP) | #416 merged through bundle #438 (`scenes/edit_spot.cpp`, `ShiftRegions`). The WIP adds two other features: peak files (`common/audiofile/peak_file.*`, `FilePeakSource`, ADR-0009 update "peak files") and Insert/Delete Time (`edit.insert_time`, `edit.delete_time`, `follow_time_ops`); main has neither (`peak_source.h` still says "until then SyntheticPeakSource") | Kept on backup; needs splitting into its two items, a rebase onto the session_model split (#463), build, tests and screenshots |
| os-qemu-tcg | ON MAIN (committed); MISSING (WIP) | The appliance-runtime commits reverse-apply (RT tuning, preflight SIGPIPE, units). The WIP's `tools/lib/qemu-accel.sh` (TCG fallback, `sonora.slow=N` scaling every wait, exit 3 inconclusive / 77 skip) is not on main; BACKLOG "an unaccelerated TCG boot" (#293) is open | Kept on backup; needs one real TCG boot (ISO build plus QEMU without KVM) before a PR, an OS-track item |
| harness-adopt | ON MAIN (committed); SUPERSEDED (WIP) | Orchestrator PRs 1-3 on main; the WIP's `tools/harness/adopt.py` is replaced by `autoadopt.py` in open #461 | None |
| convert | ON MAIN | WIP reverse-applies (`interchange/*`); sonora-convert is #375 | None |
| cf-335 | ON MAIN | Saturator files reverse-apply; #335 | None |
| cf-deesser | ON MAIN; NOTHING (WIP) | De-esser is #334; the WIP is 3181 files of `.cfb/` build output | None |
| cf-sat | ON MAIN; NOTHING (WIP) | As cf-335; the WIP is `.cfb/` build output | None |
| main-health | ON MAIN | WIP (`test_take_checkpoint`, engine/core CMake) reverse-applies; the commits landed in #442/#444/#445 | None |
| prefs376 | ON MAIN; NOTHING (WIP) | Preferences window and record folders on main (`PreferencesWindow.qml`, `preferences.cpp`); the WIP is `m376.log`/`m376.out` | None |
| pr322 | ON MAIN | #322 merged (`editor_x11.cpp`, X11 probe identical) | None |
| passiveeq-msvc | ON MAIN | The #382 MSVC fix (`<cstdint>`, `std::uint32_t count` in `pick`) is on main, `passiveeq.cpp:5,28` | None |
| multiband | MISSING, near complete | Untracked `common/dsp/include/sonora/dsp/crossover.h` and `plugins/native/multiband/` (processor, quality bars with CPU figures, goldens, 521-line suite, host fixture); nothing like it on main or in a PR; ADR-0011 places multiband dynamics in N4 | Kept on backup; needs its research note (figures in quality.json have no source page), a passing `native_multiband` run timed with `SONORA_DSP_QUICK=1`, the README line and a BACKLOG item; a plugin agent can finish it |
| dsp-n2-noise-hum | ON MAIN | Noise/hum files identical on main (`noise.cpp`, `supply.*`, `spice_noise.*`, SPICE decks); #141 | None |
| dsp-cpu-list | SUPERSEDED; NOTHING (WIP) | `dsp-cpu.sh` now discovers processors (#366), so the one-per-line list is moot; the WIP is a stale tree (-4582 lines, deletes cab, de-esser, consolidate) | None |
| ui-session-model-split | OPEN PR | Branch is #463; the WIP's `.claude/rules/code-structure.md` and `skills/split-big-file` are byte-identical in #458 | None |
| daemon-worktree | ON MAIN (committed); NOTHING (WIP) | Merge train and per-test timeouts on main (#444); the WIP is the live daemon's untracked `orchestrator.toml`, `paused.json`, `state.json.before-train` | None (follow-up: ignore `paused.json` and `state.json.*`) |
| wt-a11bc1fd | SUPERSEDED | `offline_process.*` (ADR-0032 offline chain) replaced by Render in place / Process Selection (#432, ADR-0051) | None |
| wt-a297e353 | SUPERSEDED | Transformer extremes comment and note: the corrected account landed in other words (`native-tube-n2.md:464`) | None |
| wt-a6608115 | MISSING, partial | Setup > I/O Setup (input names, per-track input pick) and a New Session dialog, 1936 lines; main has neither I/O Setup window (BACKLOG "Hardware output paths and the I/O setup page" open, which also needs engine output paths) and has New from Template instead of this dialog | Kept on backup; 173 commits behind, needs a rebase onto #463, the output-path engine half, screenshots |
| wt-aab56845 | SUPERSEDED | Rolls switch and Input Only monitoring (session v15) landed as `rolls_on`/`input_only` (ADR-0043, `op_codec.cpp:334`) | None |
| wt-ad0d84ac | SUPERSEDED | `plugins/native/bus` (placeholder bars) replaced by `plugins/native/mixbus` (#345) | None |
| wt-aefcb37c | SUPERSEDED | Record-enable modifiers landed as `ShellModel::armClick(track, modifiers, selected)` (`shell_model.h:325`) | None |
| wt-af8b2d49 | SUPERSEDED | Automation read the chain latency early: covered by `test_plan_pdc.cpp:729` (ADR-0027 §8) | None |
| wt-afde3d19 | SUPERSEDED, idea open | Same-name units became "<desc> (2)" (#253); the WIP's boot-stable key (`alsa_card_unit`: serial then sysfs bus path) belongs to the open hardware item "whether they report a usable serial" | Kept on backup as input for that hardware item |

## What became a PR

One: the dashboard and the icon, with this note (harness context). Nothing
else is both missing and complete end to end; the partial items above stay on
their backup branches, which are not to be deleted until each is picked up or
dropped.
