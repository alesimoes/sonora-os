# Plan: split the big files by domain

Status: draft (the coordinator approves)
Owner request: 2026-11-06. Rules: `.claude/rules/code-structure.md`, skill
`split-big-file`. Refactors change no behaviour, are proved by the module's
existing tests unchanged, never share a PR with feature work, never switch a
check off. Heavy builds only through `tools/build-slot.sh` (the machine lock;
never held while waiting). One fresh branch from `origin/main` per PR.

## 1. Inventory (origin/main, 2026-11-06)

153 files over 500 lines (cpp/h/hpp/py/qml/sh, `third_party` excluded), 43 of
them over 1000, counted by newlines on main. The owner's ~164/49 included
files from branches not yet on main. Tests are about 60% of the list.

Legend: **X** = excluded now (an open PR would conflict): "after #460" (model
split, ADR-0059) or "after restructure" (#459 moves `tools/`).

### common/model (all X, after #460)
3091 `src/op_codec.cpp`, 2889 `src/apply.cpp`, 2666 `src/session_file.cpp`,
2199 `src/model.cpp`, 2150 `tests/test_session_file.cpp`, 1193
`include/.../model.h`, 891 `tests/test_automation.cpp`, 809 `operations.h`,
780 `tests/test_midi.cpp`, 710 `tests/test_apply.cpp`, 695
`tests/test_midi_chunks.cpp`, 637 `tests/test_replication.cpp`, 627
`src/sequence_file.cpp`, 588 `tests/test_tracks.cpp`, 577
`tests/measure_midi_session.cpp`, 522 `tests/test_compound.cpp`.

### engine/control
X (after #460): 3665 `src/engine_control_server.cpp`, 1726
`src/plugin_hosting.cpp`, 871 `src/mix_planner.cpp`, 2477
`tests/test_plugin_hosting.cpp`, 786 `tests/test_playback_fake.cpp`, 558
`tests/test_mix_planner.cpp`.
Free: 3937 `tests/test_control.cpp`, 2647 `tests/test_playback_control.cpp`,
1264 `src/plugin_scanner.cpp`, 1213 `tests/test_closed_take.cpp`, 1163
`tests/test_plugin_scanner.cpp`, 900 `src/preset_store.cpp`, 869
`src/closed_take.cpp`, 790 `src/playback_controller.cpp`, 788
`include/.../engine_control_server.h`, 733 `tests/test_active_folder.cpp`, 719
`tests/test_track_structure.cpp`, 683 `tests/test_preset_store.cpp`, 633
`tests/test_session_commands.cpp`, 604 `src/plan_builder.cpp`, 600
`tests/test_takes.cpp`, 566 `src/sample_edit.cpp`, 566 `src/bounce.cpp`, 566
`tests/test_bounce.cpp`, 532 `tests/test_control_socket.cpp`, 507
`tests/test_loop_take.cpp`.

### engine/core
3216 `src/session.cpp`, 1314 `tests/test_punch_points.cpp`, 1313
`tests/test_automation.cpp`, 1286 `include/.../session.h`, 1159
`tests/test_sends.cpp`, 959 `tests/test_playback_mix.cpp`, 834
`tests/test_plan_pdc.cpp`, 830 `tests/test_take_boundaries.cpp`, 675
`tests/test_session_plugins.cpp`, 651 `tests/test_loop_record.cpp`, 534
`tests/test_plugin_automation.cpp`, 527 `src/disk_reader.cpp`, 526
`tests/test_plugin_params.cpp`.

### engine/app, engine/plugin_client
1276 `app/src/main.cpp`; plugin_client: 1744
`tests/test_plugin_client_host.cpp`, 1247 `src/host_supervisor.cpp`, 969
`tests/test_plugin_client.cpp`, 544 `include/.../host_supervisor.h`.

### ui/qt
src: 2971 `session_model.cpp`, 2176 `edit_controller.cpp`, 875
`shell_model.cpp`, 865 `session_model.h`, 855 `edit_controller.h`, 849
`main.cpp`, 823 `timeline_scene.cpp`, 816 `time_format.cpp`, 619
`insert_controller.cpp`, 547 `edit_automation.cpp`.
qml: 2516 `Main.qml`, 681 `ChannelStrip.qml`, 658 `EditWindow.qml`, 552
`MixWindow.qml`.
tests: 2716 `test_edit.cpp`, 1310 `test_mix.cpp`, 1267 `test_transport.cpp`,
1179 `test_clipboard.cpp`, 1027 `test_automation.cpp`, 844 `test_keymaps.cpp`,
768 `test_inserts.cpp`, 646 `test_replica_document.cpp`, 619
`test_session_templates.cpp`, 608 `test_shell.cpp`, 600 `test_comping.cpp`, 572
`test_sends.cpp`. ui/keymap: 1729 `tests/test_keymap.cpp`, 1135
`src/keymap.cpp`. ui/client: 600 `tests/test_socket_engine.cpp`.

### contracts
3872 `api/conformance/src/conformance.cpp`, 2275
`api/testing/src/fake_engine.cpp`, 1119 `plugin/include/.../protocol.h`, 1081
`api/tests/test_wire.cpp`, 938 `plugin/include/.../host_control.h`, 926
`api/src/wire.cpp`, 901 `plugin/src/host_control.cpp`, 856
`api/tests/test_state_view.cpp`, 819 `api/tests/test_fake_engine.cpp`, 803
`api/include/.../messages.h`, 791 `plugin/src/probe_record.cpp`, 742
`plugin/tests/test_host_control.cpp`, 710 `api/include/.../command.h`, 638
`plugin/tests/test_protocol.cpp`, 627
`backend/conformance/src/backend_conformance.cpp`.

### plugins
host: 3775 `tests/plugin_driver.cpp`, 1782 `src/vst3_host.cpp`, 1745
`src/main.cpp`, 745 `tests/vst3/sonora_test_vst3.cpp`, 703 `src/lv2_host.cpp`,
699 `src/state_paths.cpp`, 602 `tests/lv2/sonora_test.cpp`.
native: 1451 `format/tests/linux/bundle_driver.cpp`; `tests/suite.cpp` of fet
1028, tube 956, opto 889, tuner 650, reverb 634, tape 588, varimu 523,
saturator 518, mixbus 513. X (after #460): 651
`analyze/tests/test_mastering.cpp`, `native/vocal_balance/tests/test_analysis.cpp`.

### other src
common: 1251 `session_files/src/session_folder.cpp`, 822
`audiofile/src/audio_export.cpp`, 807
`session_files/tests/test_session_copies.cpp`, 794 `dsp/tests/test_meter.cpp`,
784 `json/src/json.cpp`, 673 `dsp/src/triode.cpp`, 654
`audiofile/tests/test_audio_export.cpp`, 631 `dsp/src/fet.cpp`, 607
`platform/src/posix/ipc.cpp`, 571 `audiofile/tests/test_wav_writer.cpp`, 562
`platform/tests/test_ipc.cpp`, 546 `dsp/tests/test_neural.cpp`, 518
`session_files/tests/test_session_folder.cpp`, 517
`midi/tests/test_midi_smf.cpp`, 507 `dsp/tests/test_transformer.cpp`.
drivers: 750 `fake/src/fake_backend.cpp`, 682
`fake/tests/test_fake_backend.cpp`, 597 `jack/src/jack_backend.cpp`.

### tools (all X, after restructure #459)
1196 `orchestrator.py`, 943 `dev-wsl.sh`, 752 `test-write-usb-entrypoint.sh`,
642 `harness/store.py`, 613 `progress.py`, 585
`harness/tests/test_daemon_unit.py`, 580 `test-engine-entrypoint.sh`, 570
`bwf-interop.py`, 548 `harness/tests/test_merge_train.py`, 545
`harness/train.py`, 538 `test-image-config.sh`, 527 `write-usb.sh`, 524
`dsp-plots.py`.

## 2. Tiers

Difficulty S/M/L/XL x risk gives the tier. Always high risk, so always senior
(opus): RT path (`engine/core`), contracts, storage/recovery, concurrency.
Junior = haiku (mechanical test splits), pleno = sonnet, senior = opus.

## 3. The top 15 (free of open PRs, by size and risk)

| # | File (lines) | Diff | Risk | Tier | Area |
|---|---|---|---|---|---|
| 1 | `ui/qt/src/session_model.cpp` (2971) | L | med | pleno | ui/qt |
| 2 | `engine/core/src/session.cpp` (3216) | XL | high (RT) | senior | engine/core |
| 3 | `contracts/api/conformance/src/conformance.cpp` (3872) | L | high (contract) | senior | contracts |
| 4 | `engine/control/tests/test_control.cpp` (3937) | M | low | junior | engine/control |
| 5 | `plugins/host/tests/plugin_driver.cpp` (3775) | M | low | junior | plugins/host |
| 6 | `ui/qt/src/edit_controller.cpp` (2176) | L | med | pleno | ui/qt |
| 7 | `ui/qt/qml/Main.qml` (2516) | L | med | pleno | ui/qt |
| 8 | `contracts/api/testing/src/fake_engine.cpp` (2275) | L | high (contract) | senior | contracts |
| 9 | `ui/qt/tests/test_edit.cpp` (2716) | M | low | junior | ui/qt |
| 10 | `engine/control/tests/test_playback_control.cpp` (2647) | M | low | junior | engine/control |
| 11 | `plugins/host/src/vst3_host.cpp` (1782) | L | high (concurrency) | senior | plugins/host |
| 12 | `plugins/host/src/main.cpp` (1745) | M | high (IPC, concurrency) | senior | plugins/host |
| 13 | `engine/app/src/main.cpp` (1276) | M | med | pleno | engine/app |
| 14 | `engine/control/src/plugin_scanner.cpp` (1264) | M | med | pleno | engine/control |
| 15 | `common/session_files/src/session_folder.cpp` (1251) | L | high (storage) | senior | common |

Order rule: one area per PR; no two concurrent refactors in one area
(ui/qt serial: 1, 6, 7, 9; engine/control serial: 4, 10, 14; contracts
serial: 3, 8; plugins/host serial: 5, 11, 12). Different areas run in
parallel. Proof for each: the module's existing tests, unchanged; the report
gives file counts before/after and a test count equal to main. Split moves
first (commit 1, byte for byte), include fix (2), interfaces (3). New files
are listed in the same CMake target.

### 3.1 `ui/qt/src/session_model.cpp` (item 1, executed first)
Domains found: constructor and `installCommands` (730 lines of lambdas, by
topic: edit modes/tools/zoom, track menu and playlists, names/colours/VCA,
clip gain/groups/sample edit, comping/consolidate/repeat/spot, memory
locations, mix/edit groups, file/save/template commands); `sync` and live
updates; undo history and event list; window configs and markers; property
getters; edit-window gestures and counters; file (save, save as, open,
recording recovery); templates; mix window; scene builders.
Target (all `SessionModel` members, public header unchanged apart from
private declarations): `session_model.cpp` (ctor, sync, live updates),
`session_model_commands_*.cpp` (by topic, each one `installXCommands`),
`session_model_history.cpp`, `session_model_state.cpp`,
`session_model_edit.cpp`, `session_model_save.cpp`, `session_model_open.cpp`,
`session_model_templates.cpp`, `session_model_mix.cpp`. Interfaces: none
extracted; the members share `ctl_`/`doc_` and there is no seam that cuts real
coupling. Proof: `tools/build-ui.sh`, all UI tests, same count as main.

### 3.2 `engine/core/src/session.cpp` (2)
Split by audio-path domain: setup/prepare, process (playback), record/capture,
punch and loop, automation, plugin chain, mix/sends/PDC, snapshots. The RT
call graph must not change: no new virtual call, lock or allocation under
`Session::process()`; hot helpers stay inline in the same unit or an internal
header. `rt-safety-auditor` checks apply. Proof: engine/core tests plus the
ASan/TSan lanes through `tools/build-slot.sh`. `session.h` changes only to
move private declarations into per-domain internal headers.

### 3.3 `conformance.cpp` (3) and `fake_engine.cpp` (8)
Conformance cases go one file per family (transport, tracks, takes, mix,
plugins, session files) behind a registry header; the runner stays.
`fake_engine` splits per command family with a shared state header. No
signature in `contracts/` changes (ADR-0007 s6). Proof: contracts tests and
the conformance suite against the fake and the real engine.

### 3.4 Tests (4, 5, 9, 10)
One test file per feature, shared fixtures to `*_fixture.h`; bodies and names
byte for byte so the case count matches main. Proof: the target's ctest count
before and after.

### 3.5 `edit_controller.cpp` (6), `Main.qml` (7)
The controller already has `edit_*.cpp` siblings: move the rest by feature
(selection, trim, nudge, fades, duplicate/repeat) and extract a small
`EditState` interface only where tests need a fake. `Main.qml`: one component
file per window or panel, root keeps the wiring; screenshot scenes must be
byte-identical.

### 3.6 `vst3_host.cpp` (11), `plugins/host/src/main.cpp` (12)
Split the host process: argument parsing, IPC loop, plugin lifecycle,
state/presets, audio callback. The audio callback and parameter queues are
not reordered. Proof: plugin host tests and the TSan lane.

### 3.7 `session_folder.cpp` (15)
Split by storage concern (layout, copy, recover, lock). Write order and
fsync points are untouched; the recovery tests must pass unchanged.

## 4. After #460 / after restructure
Re-run the inventory and plan the excluded files with the same method (model
families already split by #460; the rest of `common/model/tests` by feature).
`tools/` is planned once #459 has moved it.
