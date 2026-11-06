# Plan: separate the product from the AI infrastructure (repo layout)

Status: plan only, no file moved. Owner approved the layout 2026-11-06.
Facts come from `git ls-tree` and `git grep` on origin/main at ef79d687
(2026-11-06) and from the open PRs listed in section 7.

## 1. Goal

Today `tools/` mixes three things: build inputs and tests of the product,
the CI helpers, and the whole AI-agent harness (orchestrator, dashboard, PR
queue, session hook). `planning/` mixes the product roadmap with the
orchestrator's runtime state and the agents' handoffs. After the move:

- the product (DAW and OS) and its build/test/dev tooling live in `tools/`
  under four sub-folders and never call anything in `ai/`;
- everything that exists only to drive agents lives under `ai/`; `.claude/`
  stays at the root because Claude Code requires it;
- no behaviour change: one pure-move PR, `git mv` plus reference edits.

Dependency rule: `ai/` may call `tools/ci/*` (pre-pr, local-ci, build-slot,
merge-checks, progress) and read `planning/`; `tools/` never calls `ai/`.

## 2. Final tree

```
.claude/                  agents, skills, settings.json, OPERATIONS.md (stay)
ai/
  orchestrator/           orchestrator.py harness.py harness_mcp.py harness/ (package, tests, hooks)
  dashboard/              dashboard.py dashboard.html dashboard-up.sh
  scripts/                bg-status.sh enqueue-pr.sh merge-when-green.sh session-start.sh
  state/                  orchestrator runtime state (git-ignored) + the two tracked *.schema.json
  handoffs/               README.md, <branch>.md, main-session.md, AGENT-QUEUE.md
engine/ ui/ plugins/ common/ contracts/ interchange/ drivers/ keymaps/
iso/ audio-stack/
docs/ brain/ planning/    planning keeps ROADMAP BACKLOG PROGRESS STATUS progress.json plans/ research/ reviews/
tools/
  build/                  build-*, Dockerfile.*, rtsan.supp, entrypoint, iso-preflight, rtsan-jack
  test/                   test-*, verify-recording.py, probes, measurements, dsp tools
  dev/                    developer-machine helpers (WSL, USB stick, QEMU run)
  ci/                     local-ci, build-slot, pre-pr, merge-checks, deps and contracts checks, clean, progress
```

## 3. File-by-file move table

`git ls-tree -r --name-only origin/main tools planning` lists 131 files under
`tools/` and 9 under `planning/` outside `research/` and `reviews/`. All are
classified. Judgement calls are marked (J) and explained in section 9.

### tools/build/

| From `tools/` | To |
|---|---|
| Dockerfile.build, .bwfreaders, .labwc, .labwc-input, .livebuild, .plugins, .qemu, .rtsan, .runtime, .systemd-test, .ui, .usbtest, .yabridge | `tools/build/` same names |
| build-engine.sh, build-iso.sh, build-plugins.sh, build-rtsan.sh, build-ui.sh, build-ui-entrypoint.sh, build-portable.ps1 | `tools/build/` |
| rtsan.supp | `tools/build/` |
| rtsan-jack.sh (J) | `tools/build/` |
| iso-preflight.sh (J) | `tools/build/` |

There is no `*.cmake` file in tools/ today; the classifier patterns that
name `tools/*.cmake` become `tools/build/*.cmake` and stay valid.

### tools/test/

| From `tools/` | To |
|---|---|
| test-ab-updates.sh, test-alsa-loopback.sh, test-boot-qemu.sh, test-build-slot.sh, test-bwf-interop-record.sh, test-bwf-interop.sh, test-dev-wsl.sh, test-display-fallback.sh, test-engine-docker.sh, test-engine-entrypoint.sh, test-export-interop.sh, test-image-config.sh, test-install-qemu.sh, test-installer-tty.sh, test-installer.sh, test-iso-preflight.sh, test-labwc-input.sh, test-labwc-kiosk.sh, test-merge-checks.sh, test-policycheck-qemu.sh, test-quantum-entrypoint.sh, test-quantum-pin.sh, test-ro-root.sh, test-rootfs.sh, test-rt-limits.sh, test-rt-tuning.sh, test-secure-boot.sh, test-slots-loop.sh, test-slots.sh, test-sonora-acceptance.sh, test-umc-measure.sh, test-umc-profile.sh, test-umc-wsl.ps1, test-updates.sh, test-virtualcard-qemu.sh, test-write-usb-entrypoint.sh, test-write-usb.ps1, test-write-usb.sh, test-yabridge.sh | `tools/test/` |
| verify-recording.py, bwf-interop.py, failed-take-quit.py, crash-recovery-kill.py | `tools/test/` |
| jack-quantum-probe.py, umc-ipc-probe.py | `tools/test/` |
| umc-measure.sh, umc-wsl.ps1, umc-profile-match.lua (J) | `tools/test/` |
| dsp-cpu.sh, dsp-plots.py | `tools/test/` |
| labwc-vinput.c (J) | `tools/test/` |

### tools/dev/

dev-wsl.ps1, dev-wsl.sh, run-sonora.sh, sonora-wsl.cmd, write-usb.ps1,
write-usb.sh, make-stick-image.sh, run-qemu.sh, sync-audio-stack.sh ->
`tools/dev/`. `tools/sonora-wsl.cmd` also keeps a forwarding stub (section 6).

### tools/ci/

| From `tools/` | To |
|---|---|
| local-ci.sh, build-slot.sh, pre-pr.sh, merge-checks.sh, check-deps.sh, contracts-lock.sh, clean.sh | `tools/ci/` |
| module-deps.allow, module-deps.known | `tools/ci/` (check-deps.sh reads them next to itself) |
| progress.py, progress.sh (J) | `tools/ci/` |

Decision on progress: `progress.py` validates and renders
`planning/progress.json`, the product roadmap catalogue, and CI runs
`--check`, `--markdown` and `set` on main. It is product planning tooling,
not agent plumbing, so it stays on the product side. A `planning/tools`
folder would be a fifth tool location for two files; `tools/ci/` already
holds the other repository-state checkers. Agents keep calling it through
`tools/ci/progress.sh`.

### ai/orchestrator/

| From | To |
|---|---|
| tools/orchestrator.py, tools/harness.py, tools/harness_mcp.py | `ai/orchestrator/` |
| tools/harness/ (broker, brokerhost, client, flow, forge, heartbeat, launcher, localci, mailbox, numbers, plans, protocol, queueimport, scheduler, shadow, status, store, train `.py`, `__init__.py`, `hooks/settings.template.json`, `tests/` with 19 files) | `ai/orchestrator/harness/` same internal layout |

The three entry points and the package keep their relative positions, so
every `sys.path.insert(dirname(__file__), ...)` and `launcher.TOOLS_DIR`
keeps working (section 5.4).

### ai/dashboard/, ai/scripts/

- tools/dashboard.py, dashboard.html, dashboard-up.sh -> `ai/dashboard/`
- tools/bg-status.sh, enqueue-pr.sh, merge-when-green.sh, session-start.sh -> `ai/scripts/`

### From planning/

| From | To |
|---|---|
| planning/orchestrator/assignments.schema.json, plan.schema.json (tracked) | `ai/state/` (J) |
| planning/orchestrator/ runtime files, all git-ignored: state.json, state.quarantine.json, state.db*, events*.jsonl, status.json, STOP*, waits-on-owner.md, shadow.log, plans/, orchestrator.toml if present | `ai/state/` (moved by hand at cutover, 7a) |
| planning/handoffs/README.md and the per-branch handoffs | `ai/handoffs/` |
| planning/handoffs/main-session.md (git-ignored) | `ai/handoffs/main-session.md` |
| planning/AGENT-QUEUE.md | `ai/handoffs/AGENT-QUEUE.md` |
| ROADMAP.md, BACKLOG.md, PROGRESS.md, STATUS.md, progress.json, research/, reviews/ | stay |

STATUS.md stays: it is the product status that `session-start.sh` reads.

### Root `2048`

5 bytes, content `stub`. `git log` shows one commit, f4daa342 ("OS installed
system ... (#398)"), which added it by accident. Nothing references it as a
path. Plan: `git rm 2048` in the move PR; not a move.

## 4. Path-depth rule (the largest mechanical edit)

About 55 scripts derive the repo root as `"$(dirname "${BASH_SOURCE[0]}")/.."`
(or `dirname "$0"`/`..`). Each moves one level deeper, so `/..` becomes
`/../..`. Find them with:

    git grep -n -E '(BASH_SOURCE|dirname "\$0").*\.\.|\$SELF/\.\.|dirname "\$SELF_DIR"' -- tools

Cases with a different shape:

- `local-ci.sh:55` `ROOT=...$(dirname "$SELF_DIR")` needs a second dirname;
  the `SONORA_LOCAL_CI_ROOT` override is unchanged.
- `check-deps.sh:21` and `contracts-lock.sh:18` use `$SELF/..` (their `--test`
  modes go through `SONORA_DEPS_ROOT` / `SONORA_LOCK_ROOT`).
- `progress.py:48` two dirnames -> three. `dashboard.py:24` `..` -> `../..`.
  `orchestrator.py:1071` `--root` default two dirnames -> three
  (ai/orchestrator -> ai -> root).
- `session-start.sh:12` fallback `/..` -> `/../..`.
- `crash-recovery-kill.py:37` `HERE`; grep every `__file__` in `tools/test`.
- Harness tests that assume tools/ sits directly under the root:
  `support.py:9`, `fake_claude.py:105`, `test_e2e.py:15`,
  `test_localci_passed.py:5`, `test_escalation.py:12`, `test_daemon_unit.py:531`
  (`dirname(support.TOOLS)` used as the repo root), `test_daemon_run.py:70`
  (fixture `planning/orchestrator`).
- Sibling calls: build-iso/-plugins/-rtsan and test-boot-qemu/-bwf-interop/
  -install-qemu/-quantum-pin/-write-usb call `$(dirname ...)/build-slot.sh`
  -> `.../../ci/build-slot.sh`. `merge-when-green.sh:17` sources
  `merge-checks.sh` -> `$(dirname "$0")/../../tools/ci/merge-checks.sh`.
  `test-merge-checks.sh:5,86,109,125` source merge-checks.sh (now `../ci/`)
  and run merge-when-green.sh (now `../../ai/scripts/`).
  `enqueue-pr.sh:13` (merge-when-green next to itself), `progress.sh:8` and
  `local-ci.sh:297` (`$SELF_DIR/build-slot.sh`) stay valid.
- Hard-coded: `test-engine-docker.sh:84` `"$ROOT/tools/build-slot.sh"`;
  `local-ci.sh:261` `bash "tools/$t.sh"`.

## 5. Reference updates

Counts are lines on origin/main, excluding BACKLOG, progress.json and specs.
Master grep:

    git grep -n -E 'tools/|tools\\|planning/(orchestrator|handoffs|AGENT-QUEUE)|session-start' -- . ':!planning/BACKLOG.md' ':!planning/progress.json'

and, for scripts named without directory:

    git grep -n -E '(^|[^A-Za-z0-9_-])(build-slot|merge-checks|pre-pr|local-ci)\.sh' -- .

### 5.1 Live code (must be exact)

| File | What changes |
|---|---|
| `.claude/settings.json` (1) | SessionStart `tools/session-start.sh` -> `ai/scripts/session-start.sh` |
| `ai/scripts/session-start.sh` | handoffs `planning/handoffs/` -> `ai/handoffs/` (32-48), `tools/progress.sh` -> `tools/ci/progress.sh` (69), `tools/dashboard-up.sh` -> `ai/dashboard/dashboard-up.sh` (72), root fallback (12), self-name in the echo (25) |
| `.github/workflows/ci.yml` (60) | 5.2 |
| `.github/workflows/images.yml` (23) | `paths:` and `dockerfile:` `tools/Dockerfile.*` -> `tools/build/`; `test-engine-entrypoint.sh`, `verify-recording.py`, `test-quantum-entrypoint.sh`, `jack-quantum-probe.py` -> `tools/test/` |
| `tools/build/Dockerfile.runtime` (6) | `COPY tools/...` lines 85-89 -> `tools/test/...`; docker context stays the repo root |
| every `docker build -f tools/Dockerfile.X` | -> `tools/build/Dockerfile.X`. Find: `git grep -n -E 'Dockerfile\.[a-z-]+' -- tools .github` |
| `tools/ci/local-ci.sh`, `pre-pr.sh` | 5.3 |
| `ai/orchestrator/harness/forge.py:227,372-375` | `tools/enqueue-pr.sh` -> `ai/scripts/enqueue-pr.sh`; `tools/pre-pr.sh` -> `tools/ci/pre-pr.sh` (path and message) |
| `harness/localci.py:199` | `root/tools/local-ci.sh` -> `root/tools/ci/local-ci.sh` |
| `harness/plans.py:18` | schema dir `planning/orchestrator` -> `ai/state` (depth too) |
| `harness/flow.py:219-220` | prompt text schema paths -> `ai/state/...` |
| `harness/queueimport.py:1` and default path | `planning/AGENT-QUEUE.md` -> `ai/handoffs/AGENT-QUEUE.md` |
| `orchestrator.py` 49-51, 449, 456, 490, 752, 801, 1071, 1111 | state dir -> `ai/state` (two builders plus `odir`); prompt texts `tools/pre-pr.sh`, `tools/local-ci.sh` -> `tools/ci/`; `--root` depth |
| `harness/hooks/settings.template.json` | grep for `tools/` |
| `ai/dashboard/dashboard.py` 24, 96, 203 | ROOT depth; regex for `tools/merge-when-green.sh` in `ps` lines -> `ai/scripts/` (accept both during transition); status path -> `ai/state/status.json` |
| `ai/dashboard/dashboard-up.sh` 9, 18, 27 | depth; the hard-coded `agent-a7018d355a2572f2a` worktree status path -> `$root/ai/state/status.json` (R6); `python tools/dashboard.py` -> `ai/dashboard/dashboard.py` |
| `ai/scripts/bg-status.sh` 5-19 | `ps` greps for `tools/merge-when-green.sh` -> `ai/scripts/` (accept both during transition) |
| `ai/scripts/merge-when-green.sh` 4, 17, 19 | source path, usage text |
| `tools/ci/progress.py` 3-21, 48-50, 433, 473, 600-602 | depth; usage strings; the generated header of PROGRESS.md (R7) |
| `tools/dev/dev-wsl.ps1`, `README.md` (14) | sonora-wsl.cmd path (6.1) |
| `.gitignore` (19) | `planning/orchestrator/*` runtime lines (89-97) -> `ai/state/...` (runtime names only, so the two schemas stay tracked); `/planning/handoffs/main-session.md` -> `/ai/handoffs/main-session.md`; comments naming build-ui, build-iso, verify-recording, clean, session-start |

### 5.2 `.github/workflows/ci.yml`

| Lines | Change |
|---|---|
| 47-50 | comment: merge-when-green path |
| 63-71 image `pair` list | `tools/build/Dockerfile.*` |
| 112-131 changed-path classifier | `tools/harness/*\|tools/harness*.py\|tools/orchestrator.py` -> `ai/*`; `tools/dashboard*` -> covered by `ai/*`; `planning/orchestrator/*` -> covered by `ai/*`; `tools/build-*` -> `tools/build/*`; `tools/test-engine-docker.sh`, `test-bwf-interop*.sh`, `test-quantum-pin.sh`, `failed-take-quit.py`, `crash-recovery-kill.py`, `verify-recording.py` -> `tools/test/...`; `tools/rtsan-*` -> `tools/build/rtsan-*`; `*-entrypoint.sh` -> both `tools/build/` and `tools/test/`; `Dockerfile*\|*.cmake\|*.supp` -> `tools/build/`; `tools/*.ps1` -> `tools/*/*.ps1`; `tools/*) ;;` stays last |
| 156, 302 | `for f in tools/*.sh tools/*.py ...` and `shellcheck tools/*.sh ...` are shell globs that do not recurse -> `tools/*/*.sh ai/scripts/*.sh ai/dashboard/*.sh` (and `.py` as listed); check the `source=` directives of merge-checks.sh and iso-preflight.sh |
| 178-196, 242-260 | `python3 tools/progress.py` -> `tools/ci/progress.py` (9 places) |
| 281, 289, 296 | `dockerfile: tools/Dockerfile.*` -> `tools/build/` |
| 308-386 (23 steps) | `check-deps.sh`, `contracts-lock.sh` -> `tools/ci/`; `tools/test-*.sh` -> `tools/test/` |
| 389-400 | `tools/sync-audio-stack.sh` -> `tools/dev/` (comment and "fix with" message) |
| 417 | `unittest discover -s tools/harness/tests` -> `ai/orchestrator/harness/tests` |
| Portable (MSVC) job | explicit `--target` list unaffected; check whether it names `build-portable.ps1` |

### 5.3 Local CI classifier and pre-pr

`local-ci.sh` lines 7-12, 28-30, 39, 53, 76 (comments, usage), 108-137
(classifier), 239-264 (steps), 277-279, 297:

- 108 `planning/*|brain/*|docs/*|*.md|.github/*|.claude/*|tools/harness/*|tools/harness*.py|tools/orchestrator.py`
  -> add `ai/*`, drop the three `tools/harness*`/orchestrator alternatives.
- 109 `tools/dashboard*|tools/*.lua` -> `tools/test/*.lua`.
- 111 `iso/*|tools/build-iso.sh|tools/test-*-qemu.sh|tools/make-stick-image.sh`
  -> `iso/*|tools/build/build-iso.sh|tools/test/test-*-qemu.sh|tools/dev/make-stick-image.sh`.
- 131 `tools/*.ps1` -> `tools/*/*.ps1`.
- 133-136 the build/test/rtsan/entrypoint/Dockerfile/supp/local-ci arms get
  the new folder.
- 137 `tools/*) ;;` stays last. Danger: in `case`, `*` matches `/`, so an
  un-updated specific pattern silently falls into this arm and a PR runs
  fewer tests than it should. Hence the equivalence test in 8.2.
- 239-264: build-ui, build-plugins, build-rtsan, test-engine-docker,
  test-quantum-pin, test-bwf-interop, build-iso, failed-take-quit,
  crash-recovery-kill, `tools/$t.sh` (261) and `build-portable.ps1` (264).

`pre-pr.sh` (29-45): the `case` on `tools/*.sh` already matches nested paths,
and `grep -E '^tools/.*\.sh$|...'` (40) too; add `^ai/.*\.sh$`. Line 45
`ls tools/*.sh` does not recurse: use the new globs plus `ai/scripts/*.sh`
and `ai/dashboard/*.sh`. Also check its greps for `planning/orchestrator`
and the file-size warning from #452. Every `git mv` keeps mode 100755;
verify with `git ls-files -s -- '*.sh' | awk '$1 != "100755"'`.

### 5.4 Python imports and sys.path

The package stays next to its entry points, so no `import` changes:

- `orchestrator.py:21`, `harness.py:19`, `harness_mcp.py:12` insert
  `dirname(__file__)` (and `/harness`): valid inside `ai/orchestrator/`.
- `launcher.TOOLS_DIR` (launcher.py:25) resolves to `ai/orchestrator` and is
  used for `harness_mcp.py` (171) and `harness.py` (194): valid. Do not
  rename the constant in a pure-move PR.
- `dashboard.py` reads files and `ps`; confirm with `git grep -n import
  ai/dashboard/dashboard.py` that it does not import `harness`.
- Agent text naming `python tools/harness.py recv` -> `ai/orchestrator/`.
  Find: `git grep -n -E 'harness(_mcp)?\.py|orchestrator\.py' -- .claude CLAUDE.md README.md brain/decisions/ADR-0047-harness-orchestrator.md`.
  The per-run hook and MCP settings embed absolute paths generated at launch;
  no file edit.

### 5.5 State and the daemon

`planning/orchestrator` appears in `orchestrator.py`, `plans.py`, `flow.py`,
`dashboard.py`, `dashboard-up.sh`, `.gitignore:89-97`, `ci.yml:125` and the
tests' fixtures: `git grep -n 'planning.*orchestrator' -- tools .github .gitignore`.
New constant: `<root>/ai/state`. `.harness/` and `.harness-lci/` at the root
are not under planning/ and do not move. The daemon's restart command changes
from `python tools/orchestrator.py ...` to
`python ai/orchestrator/orchestrator.py ...` with the same flags; the move PR
updates `.claude/OPERATIONS.md`.

### 5.6 Documentation and agent text

| Where | Hits | Action |
|---|---|---|
| `CLAUDE.md` | 19 | Layout table (add `ai/`, describe tools sub-folders); Build lines; session hook; `build-slot`, `enqueue-pr`, `merge-when-green`, `bg-status`, `pre-pr`, `progress.sh`; `planning/handoffs`, `planning/AGENT-QUEUE.md`. By hand; one place explains the layout |
| `.claude/agents/*.md` | 16 files, about 70 | mechanical sed from the move table |
| `.claude/skills/*/SKILL.md` | build-engine 1, build-iso 5, handoff 8, test-loop 10 | by hand; the handoff skill is how sessions resume |
| `.claude/OPERATIONS.md` | 13 | by hand |
| READMEs: root 14, `iso/README.md` 28, `audio-stack/README.md` 4, `plugins/native/README.md`, `plugins/native/format/third_party/dpf/SONORA.md`; `planning/ROADMAP.md` 6, `STATUS.md` 12, `handoffs/README.md` 4, `PROGRESS.md` (generated) | | sed |
| `iso/staged.list` (11), `iso/auto/config`, `iso/config/hooks/*`, `iso/config/includes.chroot/*` (about 20 comments), `audio-stack/*.conf` | | sed; `iso/staged.list` is read by test-image-config.sh: check which columns it compares before editing, then run that test |
| `CMakeLists.txt` (4), `plugins/host/CMakeLists.txt` (9), `plugins/native/*/CMakeLists.txt` (5), engine, drivers, contracts conformance CMake; about 30 C++ sources/headers (1-2 hits each); `plugins/host/tests/yabridge_wine.sh` | | mostly comments; list the functional ones first with `git grep -n 'tools/' -- '*CMakeLists.txt' '*.cmake'` |
| `contracts/CONTRACTS.lock` and three contract headers (`backend_conformance.h`, `alsa_device.h`, `fake_backend.h`) mention `tools/` | | Editing a header comment changes the lock hash (R8). Default: leave those three comments for a later contracts PR |
| `brain/` ADRs (about 100 hits, 20 files), postmortems (15), research (about 80), specs (4), `docs/` | | Do not rewrite accepted ADRs, postmortems or research: they record the path at the time. Add one ADR (number reserved by the coordinator) "Repository layout: product and AI infrastructure" with the old-to-new table, its line in `brain/INDEX.md`, and a one-line note under ADR-0047's Summary that the orchestrator and harness are now in `ai/orchestrator/` |
| `planning/BACKLOG.md` (54), `planning/progress.json` (40) | | The implementer does not edit them (CLAUDE.md). Left as history; the coordinator's chore PR may sed BACKLOG afterwards |

## 6. Compatibility shims and when they go

1. `tools/sonora-wsl.cmd` (the owner's desktop shortcut target). The real file
   moves to `tools/dev/sonora-wsl.cmd`; the old path keeps a 2-line stub
   (`@call "%~dp0dev\sonora-wsl.cmd" %*`). Check that the real file computes
   its root with `%~dp0..` and change to `%~dp0..\..`; `dev-wsl.ps1` and the
   README use the new path. The owner can retarget the shortcut (Properties >
   Target) whenever. Default: keep the stub, it costs one file; it is the only
   file allowed in the `tools/` root, recorded in the layout ADR.
2. Harness fallback for in-flight branches. `forge.py` runs `pre-pr.sh` and
   `enqueue-pr.sh`, and `localci.py` runs `local-ci.sh`, inside task
   worktrees cut before the move, which still have the old tree. For one
   week the harness resolves each helper as "new path if it exists, else the
   old one". Remove when `git branch -r --no-merged origin/main` shows no
   branch cut before the cutover (target 7 days). This is the only added
   logic.
3. `bg-status.sh` and `dashboard.py` accept both `tools/merge-when-green.sh`
   and `ai/scripts/merge-when-green.sh` in `ps` output while watchers started
   before the cutover are alive; remove when the merge queue is empty after
   cutover.
4. No stubs for other scripts: an agent calling an old name must fail loudly,
   not silently run an old copy.

## 7. Order of execution

1. Wait for every open PR touching `tools/`, `planning/` or `.claude/`:
   #446 (plugin package), #452 (modular-code rule, edits pre-pr), #454
   (conformance flake), #455 (local CI lock), #457 (harness known bugs,
   build-slot, daemon guard), #458 (process PR, branch
   `process/agents-context-rules-1106`), plus #456 (harness-engineer agent,
   `.claude/agents`) and #453 (ASan build inputs), which also touch these
   paths. The move PR opens only when
   `gh pr list --state open --json files` shows none of them. During the
   wait no new agent is launched on `tools/` or `planning/AGENT-QUEUE.md`.
2. The coordinator reserves one ADR number for the layout (the ADR travels
   with the code).
3. One PR by `refactor-engineer`, fresh branch from origin/main, one commit
   per stage so a stopped agent loses nothing:
   1. `git mv` of every file in section 3 plus `git rm 2048`; no edit.
      `git diff -M --stat origin/main` must show only 100% renames and the
      one deletion.
   2. Depth edits (section 4).
   3. Live-code references (5.1-5.5) including both classifiers.
   4. Docs, agent text, skills, READMEs (5.6), layout ADR.
   5. Shims (6); run `tools/ci/contracts-lock.sh`; PROGRESS.md per R7.
   Rule: no function renamed, no logic changed, except the shim lookup in 6.2
   and the ROOT sanity check in R2. One full build cycle only (8.1).
4. The PR goes into the merge queue when opened (`enqueue-pr.sh` run by the
   coordinator from main's checkout, old path at that time).
5. After merge: daemon cutover (7a), then launch the next queue items. Every
   other branch rebases on origin/main; git rename detection carries edits to
   moved files (small files such as `module-deps.known` may fall under the
   similarity threshold and conflict).

### 7a. Daemon cutover

The daemon runs from its own checkout and keeps untracked state in
`planning/orchestrator`.

1. Precondition: the builder is idle and no builder task is mid-run. Drain
   (`orchestrator.py drain`, ADR-0047) and wait until `status.json` shows 0
   working.
2. Stop: `python tools/orchestrator.py stop` (old path, still the running
   checkout). Confirm with `ps -ef` that the process is gone.
3. In the daemon checkout: update to origin/main (its usual step). Back up
   `planning/orchestrator` to scratch. Then move the untracked runtime files
   into `ai/state/` (state.json, state.quarantine.json, state.db*,
   events*.jsonl, status.json, waits-on-owner.md, shadow.log, STOP*, plans/,
   orchestrator.toml); git already moved the two schemas. Remove the empty
   `planning/orchestrator`.
4. Restart: `python ai/orchestrator/orchestrator.py --live` with the previous
   flags. Within a minute `ai/state/status.json` refreshes and the dashboard
   shows the pools.
5. Rollback: stop, check out the previous commit, copy the state back from
   the backup, restart from `tools/orchestrator.py`. State format is
   unchanged, so this is lossless.
6. Worktrees cut before the cutover keep working through 6.2 until rebased.

## 8. Verification

On the builder, once, after the push (no waiting on CI):

1. `tools/ci/local-ci.sh --dry-run`, then `tools/ci/local-ci.sh --full`
   (runs the moved build-ui, build-plugins, build-rtsan, test-engine-docker,
   test-quantum-pin, test-bwf-interop, failed-take-quit, crash-recovery and
   the portable step through their new paths). A truncated output is not a
   pass. `--os` for the iso context, or the `--static` modes where they exist.
2. Classifier equivalence (the high-risk item; a throwaway script, output in
   the PR, not committed): for every path in the old `tools/` and `planning/`
   tree, classify the old path with the old `local-ci.sh` and the new path
   with the new one and compare the context sets; same for the `ci.yml`
   classifier. Zero differences, except agent-infra files, which must now map
   to "nothing heavy".
3. Harness unit tests: `python -m unittest discover -s
   ai/orchestrator/harness/tests`, the exact command in the edited `ci.yml`.
   Same pass count as before, no new skips.
4. `tools/ci/pre-pr.sh` clean.
5. The lightweight CI jobs locally, in workflow order: `check-deps.sh --test`
   and plain, `contracts-lock.sh --test` and plain, `progress.py --check`,
   `python3 -m doctest -v tools/ci/progress.py`, and the 23 `tools/test/test-*.sh`
   steps of `ci.yml` 308-386.
6. Dashboard: `bash ai/dashboard/dashboard-up.sh`, fetch `/` and the status
   JSON with curl; the orchestrator panel shows `ai/state/status.json` data.
   Stop it afterwards (`ps -ef`).
7. Session hook: `CLAUDE_PROJECT_DIR=$PWD bash ai/scripts/session-start.sh`
   prints handoff, STATUS and progress summary; then open a fresh Claude
   session in the worktree and confirm the hook output appears.
8. Desktop shortcut: run it through the old-path stub, then run
   `tools\dev\sonora-wsl.cmd` directly.
9. Orphan scan: `git grep -n -E 'tools/(build-|test-|local-ci|pre-pr|build-slot|merge|progress|orchestrator|harness|dashboard|session-start|bg-status|enqueue|dev-wsl|write-usb|run-|make-stick|sync-audio|umc|dsp|check-deps|contracts-lock|clean|Dockerfile)'`
   and the same for `planning/orchestrator`, `planning/handoffs`,
   `planning/AGENT-QUEUE`: only deliberate leftovers (ADRs, postmortems,
   research, BACKLOG, progress.json, the two stubs).
10. Unverified until they run: `images.yml` (runs only on main, R9) and the
    PowerShell scripts (`build-portable.ps1`, `dev-wsl.ps1`,
    `test-write-usb.ps1`, `test-umc-wsl.ps1`, `umc-wsl.ps1`): run each with
    its usage flag under `powershell -NoProfile -File`.

## 9. Items that needed a judgement call

Nothing is unclassified. Recommendations:

| Item | Recommendation |
|---|---|
| `progress.py`, `progress.sh` | tools/ci (section 3) |
| `rtsan-jack.sh` | tools/build, beside build-rtsan.sh and rtsan.supp (tools/test is equally defensible) |
| `iso-preflight.sh` | tools/build; test-iso-preflight.sh in test/ sources it (test -> build is allowed) |
| `labwc-vinput.c` | tools/test; confirm who compiles it (`git grep -n labwc-vinput`) and fix that path (probably Dockerfile.labwc-input) |
| `umc-profile-match.lua` | tools/test |
| `build-ui-entrypoint.sh` vs `test-*-entrypoint.sh` | the first is the build container entrypoint (build/), the others are copied into the runtime image for tests (test/) |
| `harness/hooks/settings.template.json` | ai/orchestrator/harness/hooks |
| `planning/orchestrator/*.schema.json` | ai/state (tracked) as approved. A cleaner alternative is `ai/orchestrator/schemas/` (code-adjacent, state dir purely runtime); default: follow the owner and ignore runtime names, not the directory |
| `planning/STATUS.md`, `PROGRESS.md`, `progress.json` | stay |
| root `2048` | delete |

## 10. Risks and recommended defaults

| # | Risk | Default |
|---|---|---|
| R1 | A missed classifier pattern falls into `tools/*) ;;` and PRs silently skip tests (likely if done by eye) | The equivalence test (8.2) is mandatory and its output goes in the PR |
| R2 | A wrong number of `..` makes a script run on the wrong directory; destructive ones are `clean.sh`, `write-usb.sh`, `make-stick-image.sh`, `test-write-usb.sh` | Depth edit is its own commit; add `[ -f "$ROOT/CLAUDE.md" ] \|\| exit 2` after ROOT in those four (the one allowed addition); run each with help/dry-run |
| R3 | Cutover during a builder run loses a run, or the state moves incompletely | Drain first, backup, rollback in 7a; builder idle |
| R4 | In-flight branches lose their helpers when the daemon calls new paths | Fallback lookup 6.2; no new tool-touching agents during the window; rebase all after merge |
| R5 | Conflicts with the 8 open PRs | Section 7 step 1: wait for all of them |
| R6 | `dashboard-up.sh` hard-codes worktree `agent-a7018d355a2572f2a` as the default status source | Default becomes `<root>/ai/state/status.json`; `SONORA_ORCH_STATUS` stays the override. Assumption to confirm in 8.6: after cutover the dashboard and the daemon use the same checkout |
| R7 | The PROGRESS.md header text changes and CI forbids PRs touching PROGRESS.md; `--check-generated` may fail on the PR | Change only the generator string and run `--check-generated` before pushing; if it fails, the PR is a declared exception in its body, and CI regenerates on main |
| R8 | Editing contract header comments changes `CONTRACTS.lock` | Leave those three comments for a later contracts PR |
| R9 | Dockerfile `-f` and `COPY` paths break only on main (`images.yml` does not run on the PR) | Full local CI builds the images once; images.yml listed unverified; the coordinator watches its first run on main |
| R10 | Windows path separators in `.cmd`/`.ps1`; PowerShell 5.1 `..` handling | Stub (6.1); run each script's usage flag |
| R11 | Chat history and habits use old paths | The layout ADR carries the old-to-new table |

## 11. Tier / grade for the execution task

- Difficulty: medium-high by breadth, low by design: about 140 file moves,
  about 55 depth edits, roughly 600 reference lines in live code and docs,
  one classifier in two places, one daemon cutover.
- Risk: high. Failures are silent (skipped tests, wrong ROOT in a destructive
  script, daemon paths) and the harness launches all other work.
- Model tier: Pleno, Sonnet (owner rule: no Opus except principal-engineer
  for escalations), with a brief that makes sections 4, 5.3 and 8.2
  non-negotiable. Not Junior/Haiku: the edits are mechanical but the checks
  need judgement. A fresh Sonnet reviewer checks only the equivalence output,
  the depth edits of the four destructive scripts and the harness path diff.
  Escalate to principal-engineer (Opus) only if 8.2 shows an unexplained
  difference or the 7a rollback is needed.
- One agent, one PR, no integrator; the cutover (7a) is the coordinator's.
