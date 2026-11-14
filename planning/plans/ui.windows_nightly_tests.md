# Plan: ui.windows_nightly_tests - Qt UI tests green in "Portable modules under MSVC"

Status: approved   (approved only by architect sign-off, below)
Written by: architect   Dispatched by: lead   Architect sign-off: architect 2026-11-12
Evidence: nightly run 43025694269, 13 of 52 `ui` tests red. PR #666 (op_codec build error) is separate and lands first or alongside. Nothing here can be verified on the owner's machine (no MSVC, no Qt): verification is the nightly workflow run by `workflow_dispatch` on the PR branch.

## 1. Goal and user experience  (architect)
No end-user change. `ctest --test-dir build-ui -C Release -L ui` (nightly.yml, "Test the Qt interface") passes on Windows, with no test skipped, filtered or loosened. Out of scope: ui/tui, Linux jobs, new features.

Causes, checked against the code (C = confirmed in code, N = not confirmed, needs a log read first):
1. C (root cause found, differs from the report): `sonora_portable_ui` (root `CMakeLists.txt` ~l.167-186) depends only on targets collected under `ui/qt`, yet the portable branch also registers tests from `ui/keymap`, `ui/client` and (via ui/client or DEFER) `ui/smart_controls`: `test_smart_controls`, `test_keymap*`, `test_ui_smart_controls` are never built, so ctest reports "Not Run" with a Release/...exe.exe path (the odd path is ctest looking for a missing executable). Fix: collect targets of every directory whose tests it labels `ui`, not only ui/qt. Also confirm `ui/smart_controls` is added in the portable branch (its Qt half is a DEFER include that needs `sonora_ui_session`).
2. C: `session_folder.cpp:549` `sync_directory_(parent)` fails on Windows; default is `platform::sync_directory` (line 366). A directory cannot be opened for fsync on Windows. Check whether `portability.session_fs` (planning/plans/architecture.portability.session_fs.md, #412) already changed it: it covers folder creation and take scan on std::filesystem, NOT the directory-sync primitive. NTFS metadata is journaled, so the Windows answer is "success after FlushFileBuffers on a handle opened with FILE_FLAG_BACKUP_SEMANTICS" if permitted, else a documented no-op returning true. Never return true on POSIX without syncing.
3. N: `test_session_templates.cpp:122` write_states; likely the same sync failure or a path/permission issue.
4. N: `test_keymaps.cpp:480`: "/" and "\\" mix plus "file already exists"; likely test builds paths by string concatenation and creates a file twice; compare with `std::filesystem::path` and `generic_string()`.
5. N: `test_edit.cpp:1759` save not Ok; expected to follow from 2.
6. C (log): no fonts on the runner (QFontDatabase: Cannot find font directory), text width 0 in ui_shell:138 and ui_mix_layout:450.

Acceptance: the nightly job step "Test the Qt interface (label ui)" ends with 100% passed, run on the PR branch; the test count is not below 52 plus the 7 previously not run.

## 2. Design  (visual-designer)
N/A - no screen or faceplate changes.

## 3. Contracts and integrations  (architect + owner engineer)
No wire or header contract. `platform::sync_directory(const std::string&) -> bool` keeps its signature; only its Windows body changes (injection via `SessionFolder::SyncDirectory` already exists, so tests use a fake).
ADR: N/A for slices B, C, D (build wiring and test fixes; ADR-0071 and ADR-0008 already decide them). Slice A (durability semantics on Windows) gets a short amendment paragraph in ADR-0008, no new number. If the architect later finds Windows cannot meet "entry durable" at all, that becomes a new ADR; number reserved by the coordinator at that time.
Reserved numbers: none.

## 4. DAW core  (rt-engineer)
Slice A only: Windows body of `platform::sync_directory` (common/platform, Windows edge, in an `#ifdef _WIN32` file of its own, not in session_folder.cpp). Not on the audio thread; no RT impact. Must not weaken the POSIX path or the failure-on-error behaviour: a failed sync still fails the save loudly (a take is never silently non-durable on Linux).
Acceptance: `test_platform` sync test (fake and real tmp dir) green on Linux `ctest -L platform`; on Windows `ui_session_open` passes.

## 5. Plugins, DSP, host
N/A - no plugin or DSP code is touched.

## 6. UI  (ui-engineer)
Slice C: fix tests 3, 4, 5 after reading their logs (`gh run view 43025694269 --log-failed`): paths via `std::filesystem::path` / `QDir::toNativeSeparators`, unique temp dirs per test (`QTemporaryDir`), no assumptions of POSIX permissions. Production code changes only if a real Windows bug is shown (then report to architect).
Slice E (fonts, test-side): ui_shell:138 and ui_mix_layout:450 must not depend on system fonts. Default decision: bundle a freely licensed font with the tests (e.g. DejaVu Sans, Bitstream Vera licence, or an SIL OFL one; licence noted next to the file under `ui/qt/tests/fonts/`), registered in a shared test helper via `QFontDatabase::addApplicationFont` before the first widget, with `QT_QPA_FONTDIR` as the Linux fallback. Reason: the production app already needs a font and these tests assert real text measurement; faking the width would make them check nothing. Rejected: making assertions pass on width 0 (loosening), installing fonts in the workflow only (hidden dependency, differs per runner image), skipping on no fonts (forbidden).
Acceptance: both tests pass on Windows with no system fonts and still pass on the Linux `ui` image; the helper is under ~100 lines.

## 7. Tests  (tester)
No test is disabled or filtered. Add: (a) a CMake check that every test labelled `ui` in the portable build has its executable as a dependency of `sonora_portable_ui` (extend `tools/check-test-labels.sh` or a ctest fixture; fails at configure time, "fail loudly", not "Not Run" in CI). (b) a platform unit test for `sync_directory` on a real temp directory and on a missing directory (returns false). Labels: `platform`, `ui`. Local check (Linux, in the container, via `tools/build-slot.sh`): `tools/micro-build.sh` on the diff and `ctest -L platform`; `cmake -S . -B /tmp/x -DSONORA_PLATFORM=neutral -DSONORA_PORTABLE_UI=ON` configure only is not possible without Qt on the owner's machine: unverified locally.

## 8. QA  (tester)
Trigger `nightly.yml` via workflow_dispatch on the PR branch; read the "Test the Qt interface" step; paste the ctest summary into the PR. Re-run once to see no flakes. No screenshots (no visible change).

## 9. Quality checks
Sanitizers: N/A for B/C/E; slice A runs under the existing micro build. RT audit: not needed (not on the audio path), reviewer confirms. code-reviewer focus: A keeps POSIX durability unchanged; B adds no test filtering; E licence file present. Files under ~500 lines (the root CMakeLists.txt is long: put the collection loop in a function or `cmake/` module rather than growing it). Docs: ADR-0008 amendment.

## 10. Code standards
`.claude/rules/code-structure.md`, `testing-and-flakes.md`, `gates-and-checks.md`, `build-lock-and-machine.md`; no brand names; DI through the existing `SyncDirectory` function type.

## 11. Dependencies and order
#666 (build fix) -> one feature branch with slices B, A, C, E -> one PR.
| Slice | Agent | Difficulty | Risk | Tier | Sub-branch | Starts from | Files |
|---|---|---|---|---|---|---|---|
| B: portable_ui collects all `ui` test targets, configure-time check | harness-engineer | M | high (build/CI) | opus | ui-winnightly/cmake | main after #666 | root `CMakeLists.txt`, `ui/smart_controls/CMakeLists.txt`, `tools/check-test-labels.sh` |
| A: Windows `sync_directory` | rt-engineer | M | high (storage) | opus | ui-winnightly/syncdir | main | `common/platform/**`, ADR-0008 |
| C: tests 3,4,5 path fixes | ui-engineer | M | med | sonnet | ui-winnightly/tests | main | `ui/qt/tests/test_session_templates.cpp`, `test_keymaps.cpp`, `test_edit.cpp` |
| E: bundled test font and helper | harness-engineer | S | med | sonnet (S/med) | ui-winnightly/fonts | main | `ui/qt/tests/fonts/**`, test helper, `ui/qt/CMakeLists.txt` |
Integrator: `integrator` merges the four sub-branches in the order B, A, E, C. Slice C starts only after the log for items 3-5 is read; if they vanish after A, drop the slice and say so.
Single PR title: ui.windows_nightly_tests: Qt UI tests pass under MSVC (build wiring, directory sync, paths, fonts)

## 12. Risks and open questions
| Question | Recommended default |
|---|---|
| Can Windows fsync a directory? (assumption, unmeasured) | Try CreateFile(FILE_FLAG_BACKUP_SEMANTICS)+FlushFileBuffers; if it fails with access denied, return true (NTFS journals metadata) and document. Researcher: confirm on the runner by a one-off test step. |
| Are items 3-5 independent bugs? | Read the logs first; fix what remains after A and B. |
| Which font? | DejaVu Sans (free licence), licence file beside it. |
| Does fixing B reveal more tests that never ran (now built, may fail)? | Yes, likely; fix in the same PR, report any that need a follow-up BACKLOG line. |
| Anything verifiable locally? | No. Only Linux micro build. Say "unverified on Windows" in the PR until the dispatch run is green. |

## 13. Done =
- [ ] #666 merged
- [ ] every slice merged into the feature branch by the integrator
- [ ] tests of section 7 pass (commands and output in the PR)
- [ ] nightly dispatch on the branch: "Test the Qt interface (label ui)" 100% passed, link in the PR
- [ ] quality checks of section 9 reported
- [ ] `tools/pre-pr.sh` clean; no file past ~500 lines
