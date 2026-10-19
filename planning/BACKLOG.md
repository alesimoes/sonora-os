# Backlog

Ordered within each phase. An item is ready when someone could start it
without asking a question.

## Complete DAW (owner, 2026-10-13): critical path and parallel tracks

The product first, the OS after (`planning/ROADMAP.md`). Stages are DAW 1 to
DAW 11; the full feature list, with the Avid or Apple document each item's
behaviour comes from, is the "Complete DAW" front of `planning/progress.json`.
This section is the order. The older sections below keep their detail and
are cross-referenced, not repeated.

**Critical path:** M2 (`contract/backend`) → M3 fake backend → playback
through the monitor bus → record while rolling with in-band take boundaries
→ M4 control API with the §3 state block → Qt UI wired to M4 (DAW 2) →
parallel graph and full mixer (DAW 4) → `engine/plugin_client` with PDC
(DAW 5). The model (`common/json`, `common/model`) must land before
playback can follow edits; it is in flight.

### Track 1: engine (rt-engineer; driver-engineer for M3; auditor gates)

1. [ ] M2 backend contract, `contract/backend` (in review).
2. [x] M3 `drivers/fake` and the conformance suite, with the two port-order
       injections ("ADR-0007 follow-ups"). `feature/fake-backend`.
3. [ ] Playback: disk reader, chunk pool, residency-gated plan swap, output
       through the fixed 2-channel monitor bus (DAW 1 detail, below).
4. [ ] Record while rolling with in-band take boundaries; punch, loop,
       locate, pre-roll; input monitoring. In-band boundaries, record
       while rolling and punch in/out on the fly at block boundaries:
       `feature/record-while-rolling`. Punch points inside a block, engine
       side: PR #81; their caller, placement and command,
       loop, pre-roll and input monitoring remain.
       For the Transport window (Track 3 item 3, `feature/ui-transport`),
       which draws these unavailable and says why until they exist:
       - handlers for `transport.rewind`, `transport.fast_forward`,
         `transport.go_to_end` and `transport.toggle_loop`;
       - a locate command taking a frame (`transport.locate`, argument),
         for counter entry and memory locations;
       - pre/post-roll from the session's `transport.pre_roll`/`post_roll`
         (saved today, not run);
       - the click and count-off from the tempo map (`engine.core.click`),
         and a state-block flag saying the click is on.
5. [x] M4: control API, wire codec, state block and `StateView` per
       ADR-0007 §3, `InProcessEngine`, `FakeEngine`; removes the illegal
       edges (`contract/api`; `engine/app` → `ui/tui` stays as the
       composition root until M7, ADR-0007 M4 update item 7).
5b. [ ] M5, M6.
6. [x] Take safety in DAW 1: 4 GB limit refused and bext date (PR #16);
       periodic checkpoint (header patched, `fdatasync`, folder `fsync`)
       and a take that survives `kill -9`, repaired at the next startup
       (`feature/take-safety`). Still open: offering recovered and orphan
       takes for placement at session open (`engine.record.orphan_offer`);
       the `fdatasync` cost on a stick (ADR-0009 Unverified 8, OS 1).
7. [x] M7 process split: socket and memfd behind `common/platform`,
       headless `sonora-engine`, `SocketEngine`, `sonora-tui`, `sonora-ctl`,
       the Qt UI on the engine's document (`feature/process-split`; ADR-0007
       M7 update). Still open: the edit controller's `resolve_*` into
       `engine/control`.
8. [ ] Parallel processing graph, buses, sends, VCA, automation in the plan
       (DAW 4).
9. [ ] For the Mix window (Track 3 item 2, `feature/ui-mixer-monitor`):
       - [ ] state block layout 3 with each track's post-fader peak (L/R,
         after pan and mute) and the monitor bus's L/R peak. `Session`
         already has `bus_peak_`; it is not published. Until then the
         strips' meters are labelled IN and the bus's NO DATA, and
         pre/post-fader metering cannot be offered. Proposed in ADR-0013
         (layout 3), which needs its contract reviews before code;
       - [ ] a `ParamId` for the monitor bus gain (and track pan), so those
         ride through ParameterChange too instead of a SetMonitorBus /
         SetMix per step. ADR-0013 §3 (protocol 4: an older engine's
         decoder refuses an unknown `ParamId` and drops the client);
       - [x] FakeEngine publishes the document's gain and mute when an
         update changes them, on a Resync and when it starts, as the real
         engine does; conformance check "the mix follows the document"
         holds both (PR #71);
       - [x] a Resync applies the document's mix (ADR-0009 §4 item 3), and
         a client that leaves mid-ride has the gain of each track it was
         the last to ride put back, so a ride a lost link stranded does
         not stay in the gain atomics. A client attaching mid-ride leaves
         the ride alone (PR #71, review decision). Track 3 removed
         `MixModel::resendGains` (PR #61), which sent every gain on each
         new document and so overrode that ride; the interface now sends
         only a dropped ride's own index, for the one case the engine
         cannot see (a step crossing another interface's reorder or Open,
         `mix_model.h`);
       - [x] `PlaybackController::apply_mix` clears the solo of an engine
         track the document does not have, so an undone AddTrack of a
         soloed track no longer silences the rest. Its fader and mute stay:
         the old plan renders it until the swap (PR #71,
         `playback_control`, rolling and stopped);
       - [ ] A UI's single-index gain restore (`MixModel::dropRide`)
         can be stale if another UI makes two mix-changing edits within one
         round trip. Fix engine-side: each ParameterChange carries the
         document version it was computed against, and the engine drops
         changes older than its last apply_mix. Not a regression; the
         removed resendGains had the same race (#76 review,
         #issuecomment-6249488192 item 1).
10. [ ] Engine commands the shipped key maps bind and no engine runs yet
        (Track 3 item 4; the window says "not implemented" for each):
        `transport.go_to_end`, `transport.rewind`, `transport.fast_forward`,
        `transport.toggle_loop`, `transport.play_half_speed`,
        `track.toggle_input_monitor`. Wired in
        the interface against `FakeEngine::Config::planned_commands`
        (test_ui_keymaps); each needs its handler in
        `install_session_commands()` and in `FakeEngine::engine_commands()`
        together, and then leaves `planned_commands`. The edit operations
        (`edit.cut`/`copy`/`paste`/`duplicate`/`repeat`/`select_all`/
        `heal_separation`/`consolidate`) wait on Track 3 item 1's compound
        operation.

### Track 2: model (rt-engineer, ui-engineer review)

1. [ ] `common/json` and `common/model`, in flight on
       `feature/timeline-model`: snapshots, operations, undo, region
       operations.
2. [ ] ADR-0009 proxies before M8 merges: Unverified 3, 4, 6 as written; 5
       and 7 on the laptop, labelled proxies (ADR-0009 update 2026-10-13).
3. [ ] Session reader and writer; fades and crossfades; track types;
       templates, autosave and backups.
4. [ ] MIDI spec and ADR, then MIDI in the model (DAW 6).
5. [ ] Solo safe in `TrackMix` (session format, op codec, plan builder: a
       solo-safe track is never implicitly muted). The Mix window refuses
       Ctrl-click on solo by name until it exists (`feature/ui-mixer-monitor`).
6. [ ] Mix and plug-in undo history, separate from the edit undo (owner
       decision 2026-10-14, recorded in ADR-0009's 2026-10-14 update, item
       4): `SetMix` (fader, pan, mute, solo), `SetInsertBypass` and
       `SetInsertParams` each make a step there, and a compound's mix
       members make one step together. Edit Ctrl+Z and the edit undo table
       are unchanged. Needs its own design first: what a step stores and
       restores, its bound, whether it replicates, how a step whose insert
       or track was removed by edit undo is shown or skipped, whether
       `SetMonitorBus` belongs in it, and its key binding. Not part of the
       compound-operation implementation.
7. [x] Compound operations (ADR-0009 update 2026-10-14): PR #75, with an
       interim limit of 64 members (5 to 7.5 ms on 2,000-region playlists).
8. [x] Compound limit raised from 64 to 256 (ADR-0009 note 2026-10-18,
       `kProtocolVersion` 5): local validation of the touched range ±1 and
       its crossfades, range-local `overwrite()` and
       `drop_broken_crossfades()`, audited against the whole-playlist
       versions after every member in Debug and in the model's tests. 256
       is the largest measured limit under 10 ms on 18 × 2,000 regions
       (3.8 ms cut-like); 1,024 costs 11 to 12 ms.
8a. [ ] Toward 4,096: what is left per member is shifting the region
       vector on every split or removal (O(n) element moves). Needs either
       a region storage that splices in O(log n) or batched members
       (`ShiftRegions`, a multi-region remove), with an ADR first, since
       `Playlist::regions` is a public vector.
9. [ ] FakeEngine adds its closed takes to its document, as the control
       server does (`add_closed_take`): a "Record" compound, its notices,
       and a refused take's TakeFailed. Then UI tests see a compound arrive
       from the engine (PR #75 ui-engineer review, item 5). contracts/api.

### Track 3: UI (ui-engineer)

Against `common/model` and `FakeEngine` now; wired to M4 when it lands.

1. [ ] Arrange view: tracks, rulers, waveforms, regions; move, trim, split,
       edit commands, undo/redo; edit modes, Smart Tool, grid and nudge;
       zoom; fades; clip gain; markers.
       - [x] Fades and crossfades (`feature/ui-fades`): keys, Smart Tool
             corners, Clear; still open: a Fades dialog, batch fades.
       - [ ] A crossfade keeps the fade it hides at that end, so clearing the
             crossfade brings it back, and it still takes room from the
             other end (the interface clamps and refuses against it, naming
             it). Pro Tools drops it: crossfade plus SetFade 0, sent as one
             `Compound` (Track 2 item 7).
       - [x] common/model refuses a crossfade that overlaps the region's
             own fade at its other end, in `playlist_problem`; edge-moving
             edits, crossfade clears and punches shorten the fade instead,
             and the loader repairs older v2 files the same way, reporting
             each shortened fade on Open (PR #69).
       - [ ] Once the engine sends punch crossfades (`add_closed_take` sends
             none today), the take's report names any neighbour's fade the
             punch shortened (`RecordTake` runs `fit_fades_to_crossfades`).
             Pro Tools truncates the fade and never refuses the record; the
             operator should still hear what changed (PR #69 review, note 3).
       - [ ] Cut/copy/paste/duplicate/repeat and Shuffle: each one
             `Compound` with a label ("Cut", "Paste"), now in common/model
             (Track 2 item 7), within `model::kMaxCompoundMembers` (256 since
             Track 2 item 8). Shuffle on a long playlist needs a `ShiftRegions`
             operation first (ADR-0009 update 2026-10-14, item 5).
       - [ ] The compound rule for every sender (PR #75 ui-engineer
             review, item 4):
             - one gesture is one compound and one undo step; the UI never
               splits an operator's edit across compounds;
             - over `model::kMaxCompoundMembers` (the constant, never a
               literal) the UI refuses before sending, changes nothing and
               says so in operator words ("Paste needs 300 changes; at most
               256 in one edit. Nothing was pasted."), not the
               model's "operation 257 of 300";
             - build to the fewest members: Shuffle's ripple waits for
               `ShiftRegions` (one member a playlist), and until then a
               Shuffle edit that would ripple more than fits is refused;
             - a `ui/qt/tests` case per sender at the limit and one past it.
             256 since Track 2 item 8: an 18-track paste of about ten
             regions a track, with its overwrite splits, fits.
       - [ ] The undo and redo text: "Undo Record", "Undo Paste", from the
             history's step labels (`History::undo_label()` /
             `redo_label()`, PR #75), as a property and an Edit menu item.
2. [ ] Mixer view on the monitor bus: fader, pan, mute, solo, metering.
       - [x] `feature/ui-mixer-monitor`: fader rides heard through
             ParameterChange and saved by one SetMix at release; typed gain
             and pan in the readouts; Ctrl-fine pan; Alt-click solo/mute
             all; strips read the mix by display order (they read
             MixState's id order); meters labelled for what they read.
       - [ ] Post-fader and bus meters, pre/post switch: Track 1 item 9.
       - [x] Interrupted rides (PR #61 review): a ride holds its track id
             and session and is dropped, with the reason, if either goes
             before release (since PR #71 the engine restores the mix; a
             dropped ride resends its own index only);
             live-parameter Ok replies no longer clear the status line.
       - [ ] Alt-click on solo or mute is one SetMix per track: N revisions
             and no undo step, since mix state is not edit-undoable. It
             should be one `Compound` (Track 2 item 7): one update, no
             edit-undo step, and one mix-undo step once that history exists
             (Track 2 item 6).
       - [ ] Solo safe: Track 2 item 5. Solo modes (latch, X-OR,
             momentary) and stereo pan wait for those and stereo tracks.
3. [ ] Transport window and counters; tempo map and click display.
       - [x] `feature/ui-transport`: bars|beats through the whole tempo map
             (counters, ruler, grid lines, snap); the Transport window
             (`window.transport`) with every format from one reading, tempo,
             meter, next change and the map, pre/post-roll, loop and punch;
             click and rolls dimmed with the reason.
       - [ ] Tempo and meter rulers in the Edit window; tempo and meter
             editing (a SetSettings each; the conductor).
       - [ ] Counter entry (type a position, Enter locates): needs
             `transport.locate` from Track 1 item 4.
       - [ ] The nudge steps by the first entry's grid value
             (`edit_controller.cpp` `nudge`): step by the value at the
             region instead, after `feature/ui-fades` lands there.
       - [x] A stale engine read no longer makes the bar counter and the
             Transport window disagree (#62 merge gate): `SessionModel`
             keeps the last state on a stale read, as `TransportModel`
             does (ui_transport test_window).
4. [x] Key maps driving M4 commands, profile switch, Commands Keyboard
       Focus; fix the `keymaps/default.json` comment and its embedded copy
       (`feature/ui-keymaps`): routing by `CommandSpec::domain`, the engine
       first for Engine commands it lists; `"focus": "commands"` bindings,
       `focus.commands` on `ctrl+alt+1` and the a-z button; the Keys menu,
       remembered in `~/.config/sonora/keymap`. Still open:
       - [ ] Verify against Avid: Pro Tools' focus state in a new session,
             Command+Option+1 turning focus off, letters with focus off,
             focus in the Mix window; `comma`/`period` (nudge or
             nudge-and-play in the ch. 3 table).
       - [ ] The tty has no a-z indicator, so its focus keys stay live; a
             focus toggle there needs a footer indicator first.
       - [ ] Pro Tools' Clip List and Groups List focuses (`ctrl+alt+2`,
             `ctrl+alt+3`) once those lists exist.
       - [x] On engine reconnect the whole Hello is re-read: sample rate,
             buffer size and track count too. ShellModel resizes its peaks
             and arm list, refreshes the clock and emits
             `engineFormatChanged` (trackCount, formatText); MixModel asks
             for the track count when it uses it (PR #67 review; ui_keymaps
             test_engine_restart_format, ui_mix live_path_follows_the_engine).
             The start-up refusal notice survives a reconnect that changes
             the essentials notices (test_restart_keeps_refusal). Still
             sized from the first Hello: the synthetic waveforms' rate
             (`SyntheticPeakSource`), which only shapes made-up peaks.
5. [ ] Playlists and comping (DAW 3), full mixer and automation UI (DAW 4).
6. [ ] The version refusal reaches the operator (PR #75 ui-engineer review,
       item 3). `version_mismatch()`'s text ("the engine speaks control
       protocol version 4 and this interface speaks version 3; install an
       interface and an engine from the same build") goes to stderr and
       exit 1 today, which on the appliance is a black screen restarted
       for ever:
       - at startup, a full-screen refusal page with the text instead of
         exiting;
       - on reconnect, `last_error()` shown in the engine-gone overlay;
       - a version refusal stops the retry loop, since retrying cannot fix
         it.

### Track 4: plugins (plugin-engineer; rt-engineer for anything in `engine/`)

1. [x] Out-of-process LV2 host: scanning, validation, crash/hang/liar test
       plugins, the host loop, and the shared-memory round-trip measurement
       (PR #26; `brain/research/plugin-host-lv2-container.md`). The runtime
       canary check covers only 64 words past `max_frames`.
2. [ ] Accept `contracts/plugin/include/sonora/plugin/protocol.h`. Merged as
       a DRAFT with PR #26 (trust rule, private `Layout`, depth 1 default,
       no audio-thread wait); stays `kLayoutVersion` 0 until these land (the
       same list is the `TODO(accept)` block at the top of the header):
       - parameters: per-block-tagged changes with a frame offset, and a
         last-value table in the region;
       - crash/restart: never reuse a region a dead host touched; restart =
         new memfd + host + Ready + latency re-read, rejoining through a
         control-thread plan swap with parameters replayed;
       - plan swaps (ADR-0009): bypass atomic + RtEvent; `HostChannel` state
         keyed by stable node id outside the plan; an in-flight depth-1 block
         discarded across a chain swap; PDC across a `latency_generation`
         bump; a "dry" miss is dry delayed by `latency_frames`;
       - versioning: `header_bytes`, static_asserts on sizeof/offsetof, and
         how "incompatible" is reported;
       - the RT auditor's ruling on one `FUTEX_WAKE` per callback (depth 0
         poll-only depends on it too).
3. [ ] Plugin ADR (M9) from that measurement: depth 1 on the audio thread
       (the container saw a 2 ms timed wait return 5.9 ms late), host
       per chain/track/plugin, the wake syscall's auditor ruling, `HostControl`
       over the control socket (scan via one `--probe` per process,
       instantiate, remove, editor with the parent-window token).
4. [ ] `engine/plugin_client` and PDC, once M4 exists: `HostChannel::submit/collect`
       on the contract, hosts forked from the long-lived control thread and
       watched by pidfd (`PR_SET_PDEATHSIG` follows the forking thread),
       removal after N consecutive misses with the plugin seen running during
       the streak named to the operator, the per-plugin NaN/Inf counters
       shown; a
       2048-sample track lands aligned (ctest).
       Carried from the PR #45 reviews (ADR-0012 checklist 4):
       RT half and supervisor stub landed on `feature/plugin-client`
       (engine/plugin_client); the items below marked [x] are in it.
       - [x] RTSan CI job (Unverified 10), `ci/rtsan`: CI job `rtsan`
             (`tools/build-rtsan.sh`, clang-22). The `syscall` in
             `wake_all` IS reported (`plugin_client_rtsan_raw_syscall`
             holds that); `wake_all` runs it in an `__rtsan_disable` scope
             (A1/A3) and `plugin_client_rtsan_wake` is clean under
             `halt_on_error=true`. The reachability test allows that pair
             in wake.cpp's object only.
       - [x] RTSan over `plugin_client_host_*` and `plugins/host`
             (`ci/rtsan-jack`): lilv, the VST3 SDK headers and the Debian
             LV2/VST3 test plugins are in `tools/Dockerfile.rtsan`; the
             host's per-block work is `run_block`, `SONORA_RT`, so
             Chain::process and every plugin's run() are checked. Clean:
             eg-amp, x42-dpl, LSP limiter (LV2 and VST3), AGain, the
             native bundles. The in-tree `#sleep` plugin's sleep is the
             deadline tests' overrun and is exempt; `plugin_host_rtsan_block`
             is the control (unexempted, reported from run_block). The
             deadline tests had passed with the host halted by RTSan, so
             `tools/build-rtsan.sh` now fails any report in a passing test
             not labelled `rtsan-report`.
       - [x] Wire `PluginClient::process_chain` into the InsertChain node:
             `PluginChainRunner` (engine/core, PR #59; in engine/core
             because ADR-0007 draws core -> plugin_client, not the
             reverse). Once per node per callback with the whole block,
             `kNoStore` passed through, a chain wider than stereo silence
             and counted, a mono chain's side 1 set to side 0. "Unbound" is
             the builder's: `layout_track_graph` lays out a binding with no
             slot or no node state as no slot, latency 0 (rt-engineer
             ruling 1), and a node that still reaches the runner at
             `kNoNodeState` is silence, counted, never dry and early. A
             zero-frame callback neither begins nor ends. ctest `plan_pdc`,
             `plugin_chain_runner` (no host) and `session_plugins_*`:
             Session::process with real hosts, LV2 and VST3, Debian's and
             in-tree, mono and stereo, a 2048-frame chain of four Debian
             limiters landing bit for bit on a dry track, a stalled host
             removed while the dry track plays on and no callback waits.
             rt-engineer co-review (ADR-0007 §6): engine/core links
             sonora_plugin_client, which links common/platform PRIVATE.
       - [ ] `SupervisedChain` into the control server (checklist 5),
             with the ChainResolver filling ChainBinding from it; pidfd
             instead of polling.
       Carried from the PR #59 reviews (RT audit, rt-engineer co-review),
       out of that PR's scope and handed to checklist 5:
       - [x] The callback's timestamp (checklist 5 RT PR):
             `contracts/backend` `AudioBlock::callback_ns`, stamped by the
             backend (JACK: `jack_get_cycle_times`' current_usecs; the fake:
             its thread's steady_clock), carried forward by the period when
             0; `Session::process` reads no clock. ctest `plugin_params`
             (`callback_ns_from_the_backend`) and the conformance check
             `callback_ns_is_the_monotonic_clock` (fake in ctest; jackd2
             and PipeWire in `tools/rtsan-jack.sh`). Still open on the
             UMC1820 target: `strace -f -e trace=clock_gettime` on a playing
             `sonora-engine` shows no call from the audio thread.
       - [x] From the #59 re-review (checklist 5 RT PR): the builder's
             `PlanReport::track_latency` is the latency the plan carries,
             not the resolver's figure, so an unbound chain shows 0 (ctest
             `playback_control` `report_shows_the_planned_latency`); and a
             bound chain the session does not run (no runner, a callback the
             runner does not take) plays silence, counted in
             `Session::unrun_chain_blocks()`, never its dry input early
             (ctest `plan_pdc` `bound_chain_without_a_runner_is_silence`,
             `long_callback_bypasses_chains`).
       - [ ] One block size, one policy (auditor finding 4, rt-engineer
             item 3): pin `ClientConfig::block_frames` to the backend's
             period and set `max_chain_channels = 2`; on a period change,
             restart the hosts and replan (the chain latencies include the
             IPC block). Control-server PR. Done in the RT PR: Session's
             gate uses `InsertChainRunner::block_frames()`, so another
             period never reaches the client and is counted once, in
             `chain_bypassed_blocks` (ctest `plugin_params`).
       - [x] Parameter events into `run()` (rt-engineer item 1; checklist 5
             RT PR): `Session::post_plugin_param(store, instance, param,
             value)` into a ring of its own (`plugin_param_capacity`, so a
             drag never holds a locate back), drained at block start and
             handed to the InsertChain node naming that store as
             `run(node, in, out, frames, const ParamEvent*, n)`, in posted
             order, offset 0; what no node takes is dropped and counted
             (`plugin_params_dropped`). Gathered per node from one array the
             ring's capacity rather than per-node-state staging: the
             session then needs no `max_nodes` of the client's, and the
             cost is bounded (capacity x InsertChain nodes compares). The
             runner stays stateless. ctest `plugin_params`,
             `plugin_chain_runner`, `session_plugins_params` (eg-amp's gain
             and its bypass, each from the callback after the post).
       Carried from the #68 reviews (RT audit, code review), done in the
       RT follow-ups PR (`feature/plugin-rt-followups`):
       - [x] N1, gather cost: a store-to-node index built when the audio
             thread adopts a plan (`PlaybackConfig::max_param_stores`), and
             one counting pass per callback: O(ring + nodes), posted order
             kept per node. ctest `plugin_params`
             `gather_many_nodes_full_ring` (8 chains, scrambled stores, a
             full ring).
       - [x] N2: `PluginClient::begin_callback(0)` publishes nothing, so an
             unstamped first period never phases the hosts from epoch 0
             (ctest `plugin_client` `an_unstamped_callback_publishes_nothing`).
       - [x] Conformance: `callback_ns` checked per period against
             `steady_clock` read inside `process()` (never ahead, behind by
             under 4 periods + 5 ms), and the contract's occasional 0
             allowed (at most 1 period in 16).
       - [x] A chain not run fades out and ramps back in:
             `InsertChainRunner::skip` / `PluginClient::fade_node` (the §3
             fade of the node's last sample; the next collect ramps in).
             ctest `plugin_client` `a_node_not_run_fades_out_and_ramps_back_in`,
             `plugin_chain_runner` `skip_fades_through_the_client`,
             `plugin_params` `runner_block_size_gates_the_callback`.
       - [x] `kSilentChannelSlot` (ADR-0012 amendment of 2026-10-14, item 1,
             change A): laid out with its latency, never run, faded by
             `skip`, its changes dropped and counted; `silent_chain_blocks`.
             ctest `plugin_params` `silent_slot_keeps_its_latency`.
       - [x] RT re-audit of #60's merge: the parameter ring is drained only
             in a callback whose chains run; a zero-frame callback, one of
             another size, a split one or a plan with no chain leaves the
             changes queued for the next, instead of dropping what nothing
             re-seeds. ctest `plugin_params`
             `zero_frame_callback_keeps_changes`, `unrouted`,
             `runner_block_size_gates_the_callback`.
       - [x] PR #77 RT audit, fixed in the same PR: jackd2 at SCHED_FIFO in
             `tools/rtsan-jack.sh`, and the `callback_ns` check skips, with
             its reason, on a run with xruns or late callbacks (B1);
             `plugin::next_wake_ns` never more than a period ahead (B1,
             contract); parameter changes tagged with their plan's epoch
             (M1, `plan_swap`); the least lag checked too (M2); the first
             clock offset retried and never kept when wide (L1,
             `clock_offset`); `atomic<int64_t>` lock-free asserted (L2);
             `activate()` returns the thread's error (L3); `plan_in_bounds`
             refuses more nodes than scratch buffers (L4); the ramp-in
             starts from the fade's gain (L5).
       - [ ] Re-post or reseed when `plugin_params_dropped()` moves (a store
             no runnable node names, a change for a replaced plan), and
             coalesce-and-retry a refused post
             (ring full, including while no chain runs): the control-server
             PR.
       - [ ] After a callback the runner did not take, the next collect
             plays the host's block from before the skipped callback (one
             period late, ramped in): the client's callback count does not
             move on a skipped callback. Rare (period mismatch, a cleared
             runner); a listening check decides whether it needs more.
       - [ ] `PluginFault` from the client, not the runner (rt-engineer
             item 2): the control thread polls `slot_status(slot)` and
             `removed(slot)` for the slots the resolver assigned, so a
             fault names its chain. The runner's session-wide counters stay
             diagnostics or go, never a second source.
       - [ ] One owner for teardown (session, hosts, runner, supervisor,
             client), and `session_plugins_*` moved to engine/control once
             the resolver drives it, with a supervisor-level `idle()` in
             place of the test reading `plugin::ChannelHeader` (rt-engineer
             items 4 and 5).
       - [x] Checklist 5 REQUIREMENT (code-reviewer L6 on PR #49):
             `HostSupervisor::spawn` and `SupervisedChain::begin` return at
             once; `HostProcess::poll_startup` and `SupervisedChain::poll`
             notice Ready (Swapped), the refusal (Down, not retried) or the
             timeout (Down); a restart is `Restarting`, then a later
             `Swapped`, the old slot in the plan meanwhile. The blocking
             `start()` stays for harnesses. `SupervisedChain::idle()` and
             `HostProcess::idle()` (rt-engineer item 5). ctest
             `plugin_client_host_{async_start,ready_timeout,async_refused}`,
             and `miss` checks Restarting before Swapped with no poll over
             150 ms. Still blocking on the control thread: `HostProcess::stop()`
             (shutdown word, then up to 2 s) from `retired()` and the
             destructor; make it asynchronous with the control-server PR.
       - [x] rt-eng M5: fade/ramp state per node, keyed by the chain's
             stable id like the delay lines, not per slot, so make-before-
             break does not cut hard; the silent block a channel switch
             causes does not count toward the 8-miss streak; below quantum
             64 the 64-frame fade spans blocks. One line in ADR-0012 §3/§5.4.
       - [x] A2.2: the caller charges `value_copy_bytes` against
             `kValueCopyBudgetBytes` per callback (first submits wait,
             held-change resyncs submit without the copy and stay set).
       - [x] A2.5: the plan node carries the store's index, not a pointer
             (`PlanNode::param_store`, filled through the builder's
             ChainResolver; PDC PR, ADR-0012 checklist 3).
       - [x] A3: the reachability test (nm); the RTSan test is written, its job is the item above (no `common/platform` symbol but
             `wake_all` reachable from `Session::process`) and the RTSan
             test for `wake_all` (Unverified 10) land BEFORE the first
             `SONORA_RT` caller of `wake_all`.
       - [x] `contracts/plugin/host_control.h` (fd-3 socket messages,
             `kHostWireVersion`, `wire N` appended to `--version`) as its own
             contract PR with both reviews, before plugin_client's host
             supervisor (Hello/Ready, `GetState` for make-before-break). PR #51.
       Carried from the PR #51 rt-engineer review, for the host supervisor and
       the control-server wiring (ADR-0012 checklists 4 and 5); the contract
       text is in `host_control.h`:
       - [x] rt-engineer co-review of #78, fixed: replies matched to the
             requests outstanding, State's identity checked, unsolicited
             messages held under a cap (H1); the state caps checked before
             any copy, a state over 16 MiB that instance's refusal (M1);
             touches by a wall-time bucket that lets a stalled pump's backlog
             through, GestureEnd bursts coalesced and a kill only past ten
             times the human rate (M2); the Ready bound from the Load fully
             sent, `wants_write()`/`socket_handle()` (M3); the refusal text
             kept, one answer to Load (L1); messages of hosts let go handed
             over (L2); duplicate and high descriptor targets refused,
             file-action errors checked (L5). ctest
             `plugin_client_host_control_{rates,unsolicited,breaches,load_cap}`.
       - [x] #72's follow-ups (#issuecomment-6246382528), done in the #78
             round: one time base for the Ready deadline (set from the
             caller's clock at the first poll_startup, not spawn's own);
             `HostProcess::refused()`'s comment says what it covers and who
             reads it; tests for a host that exits before Ready, a
             replacement that never reaches Ready, and a chain destroyed
             while its host starts (ctest `plugin_client_host_control_start_paths`).
       - [ ] M3, the control server's side: poll each host's socket for
             POLLOUT while `wants_write()` (the 50 ms loop moves a Load near
             64 MiB slowly; the Ready bound now starts once it is sent).
       - [ ] L6 on #78: the Hello pid check assumes no PID namespace; a
             sandboxed host (bwrap, unshare) needs the pid as the engine sees
             it compared another way.
       - [x] HostControl in the supervisor (`feature/plugin-host-control`):
             hosts start with `--control-fd 3 --shm-fd 4 --clock-fd 5`
             (`ChildProcess::Options::handles`), EngineHello and Load
             queued before the spawn, Hello checked (versions, pid),
             Loaded checked (one entry per instance, each table the scan's
             size, latencies plausible) and required with the region's
             Ready before the chain binds; every later message decoded
             and checked (instance, dense parameter) and handed to the
             control server (`take_host_messages`); Shutdown on the socket;
             `--version` must name the wire version too. A refusal names
             its instance (`refused_instance`, for change C). ctest
             `plugin_client_host_control_{loaded,refused_instance,bad_hello,bogus,flood,half}`
             (a scripted fake host for the contract breaches),
             `platform_ipc` `a_handle_is_passed_to_a_child_beside_the_regions`.
       - [x] M2: the engine enforces the touch rates itself: at most
             `kMaxTouchesPerSecondPerParam` ParamTouched per (instance, param)
             and `kMaxGestureEndsPerSecond` GestureEnd per host in any
             one-second window. Past either, the host is a protocol fault
             (kill, crash path); ctest `plugin_client_host_control_flood`.
             The PluginFault naming the insert is the control-server PR.
       - [ ] M4: before Reconfigure, an RtCommand sets the node's slot
             "excused" flag. Misses while it is set are played (fade,
             silence) and counted, but never count toward the 8-miss
             removal. The flag clears when the plan built from Reconfigured's
             `latency_generation` is swapped in, or with the kill on
             timeout. The window after resume plays misaligned, not silent,
             until that swap (ADR-0012 §5.4). Test both edges.
       - [x] M1: kill a host when `Link::stalled(now, kStalledMessageMs)`
             (checked in every poll; ctest `plugin_client_host_control_half`).
             `HostProcess::send` refuses past `kMaxQueuedBytes`; the caller
             (control server) kills on that refusal.
       - [x] Low: a Load that passes `kMaxMessageBytes` is refused at spawn
             with "<chain>: the chain's plugin states exceed the 64 MiB Load
             cap"; the control server turns it into the PluginFault. Not
             tested with a 64 MiB state (unverified).
       - [ ] Use the contract's helpers: `EngineChannel` per slot,
             `collect_output` (count is a miss and a fault), `hold_param`,
             `submit_block`; `parse_version_line` at start-up; `mlock` each
             region and report a failure (auditor L3); refuse plugin
             hosting when `common/platform`'s SharedRegion could not add
             `F_SEAL_FUTURE_WRITE` (needs an accessor there; auditor L2).
       - Start and stop hosts from the document diff (the set of active
         insert ids before and after each change), not from the op kind:
         today `starts_a_host()` in `engine_control_server.cpp` gates only
         Apply of AddInsert and ReplaceInsert, but undo and redo of
         RemoveInsert or ReplaceInsert, and `SetInsertActive{true}`, also
         bring an insert into the chain (rt-engineer review of #46, low 4).
       - Key an instance on (insert id, channel layout) in the plan builder.
       - `InsertChainRunner` over PluginClient is `PluginChainRunner`
         (done, see above). Left for checklist 5:
         the ChainResolver fills latency_frames from
         PluginClient::latency_frames(slot) and the slot, generation,
         store and node state from the supervisor. Match the explicit
         0xFFFFFFFF "none" for param_store and node_state. A callback
         longer than max_block_frames passes every chain through unrun
         (counted); show Session::chain_bypassed_blocks() somewhere.
       - Wire Qt File > Save to send `FlushPluginState` and wait for its
         reply before serialising (ADR-0012 §8), once a host can hold state.
         Done in the UI insert-slots PR (ADR-0012 checklist 6); Alt-drag
         copy flushes the source insert the same way.
       - `active` on `AddInsert` (op codec, session-format and protocol
         change; ui-engineer and rt-engineer co-review under ADR-0007 §6).
         Until then an Alt-drag copy of an inactive insert is AddInsert
         (active) then SetInsertActive(false): two undo steps, so the
         first Undo leaves the copy active and starts its plugin, and the
         engine starts a host only to stop it. test_inserts
         `copy_of_an_inactive_insert` pins the current behaviour; flip it
         when the field lands (review of PR #50, medium).
       - Insert-slot mouse gestures per profile: Ctrl-click bypass,
         Ctrl+Start-click inactive and Alt-drag copy are Pro Tools' and
         apply under every key map, `logic.json` included. Logic's own
         (Option-click bypass on a slot, among others) need a mouse section
         in the key-map format (`keymaps/README.md`), as keys have; they
         clash with no key binding meanwhile (review of PR #50, low).
5. [ ] LV2 state save/restore, `worker:schedule`, presets; plugin manager;
       editors under WSLg (the host opens parentless X11 toplevels). VST3
       hosting is done (this PR, `brain/research/plugin-host-vst3.md`);
       VST3 state (`getState`/`setState`), instruments (event input, no
       audio input) and `hostchecker.vst3` against the host remain.
       - Legacy VST3 bundles without a `moduleinfo.json` (before VST 3.7.5)
         are loaded into the serving host itself to find a class, so their
         static constructors and factories run beside the chain's plugins
         (`brain/research/plugin-host-vst3.md`, Finding 9). Isolate them:
         resolve the class in a child, as `--list` does, and load only the
         bundle that has it.
6. [ ] **Windows VST through yabridge** (next, plugin-engineer; not started).
       What it needs:
       - Wine and yabridge (GPL-3.0) in the plugins image and, later, the
         appliance. Debian trixie main has Wine 10.0 (`wine`, checked
         2026-10-13) but no yabridge: pin an upstream yabridge release, check
         which Wine versions it supports, and record both licences.
       - `yabridgectl sync` turns Windows `.vst3` bundles into Linux `.vst3`
         bundles under `~/.vst3/yabridge`, which the VST3 host already scans
         and runs. The Windows VST2 path makes `.so` VST2 plugins, which
         this host does not load: VST2 needs Steinberg's VST2 SDK licence
         (no longer offered) or a clean-room header; decide before doing it.
       - A test plugin with a Windows build under a free licence (an LSP or
         DPF Windows VST3), so CI runs one through Wine. Measure the cost:
         yabridge adds its own IPC hop (plugin host -> yabridge -> Wine),
         which lands inside this host's block budget and must be measured
         against the 2.67 ms deadline before it is offered.
       - Crash isolation holds as is: a Wine crash ends this host process.
       - Licence checks and copy protection inside Windows plugins (iLok and
         the like) are not circumvented (ADR-0004); a plugin that needs
         them under Wine either works or is reported unusable.
       - Editors stay out of scope (ADR-0010).
7. [ ] Freeze, bounce in place, offline processing; the curated stock set.

### Track 5: dev environment (distro-engineer, researcher)

1. [ ] Measure WSLg audio from WSL2 Debian trixie: can PipeWire bridge to
       WSLg's PulseAudio server for playback **and** capture, what quantum
       holds with zero xruns over 10 minutes, round-trip latency. Assumption
       until measured. researcher.
2. [ ] `tools/dev-wsl.sh` when engine and UI are first connected: builds
       engine and UI, starts PipeWire on onboard audio, runs both. Not
       before.

### Later stages, in order (detail in `progress.json`)

DAW 6 MIDI and instruments, DAW 7 tempo/time/pitch, DAW 8 interchange,
DAW 9 Logic creative tools and content, DAW 10 surfaces/sync/video, DAW 11
surround. Researcher, before DAW 6: pin the Pro Tools Reference Guide
edition (PT-RG) and check every `ref` in `progress.json` against it and
LP-UG 12.3; flag any feature that is content rather than behaviour.

## OS packaging (after the Complete DAW)

Every section from here to "ADR-0010 display stack" that touches the image,
the USB stick, QEMU or hardware is OS packaging (ROADMAP, Milestone 2). Do
not start or extend those items before the Complete DAW closes (owner,
2026-10-13). Engine items inside them (the bext date) are pulled into DAW 1.

## Phase 1 (OS 1, after the Complete DAW)

- [ ] Compile the engine on Debian trixie and fix what the compiler finds.
- [ ] Run `ctest`; make the ring buffer and BWF tests pass.
- [ ] `live-build` config: package list, RT kernel, PipeWire from backports.
- [ ] `sonora-session.service`: autologin on tty1 into the engine, respawn on
      exit, no display manager.
- [x] PipeWire drop-in pinning quantum to 128 and rate to 48000, and holding
      it against clients: `tools/test-quantum-pin.sh`, in CI.
- [ ] Repeat `tools/test-quantum-pin.sh`'s routes with the UMC1820 as the
      graph driver instead of the dummy driver. A real ALSA node may have
      period-size constraints the container cannot show.
- [ ] Decide whether to ship `default.clock.quantum-limit = 128`, which
      closes the measured `clock.max-quantum` + `clock.force-quantum` gap but
      also sizes the server's ALSA and resampler buffers. Needs a device that
      cannot run at 48 kHz to test against. `brain/research/quantum-pin.md`.
- [ ] `limits.d` granting `rtprio 95` and `memlock unlimited` to the audio
      group.
- [ ] Udev rule naming the first class-compliant USB interface so port
      auto-connection is stable across reboots.
- [x] `tools/write-usb.sh`: write the ISO and create the persistence partition.
      Verified against a loop device and booted in QEMU, with persistence
      surviving a reboot: `tools/test-write-usb.sh`. Not yet run on a
      physical stick.
- [ ] Write a physical USB stick with `tools/write-usb.sh`, boot it on a
      physical machine, BIOS and UEFI, and confirm a session survives a
      reboot. The first time the script's USB/removable check meets real
      hardware.
- [ ] Run `tools/test-write-usb.sh` in CI. The container half
      (`WRITE_USB_BOOT=0`) needs only Docker and the ISO; the boots need QEMU
      with KVM.
- [ ] A verified way to write the stick from Windows, where the owner works.
      The first attempt (`write-usb-windows.ps1`, removed 2026-10-12) opened
      `\.\PhysicalDrive` with one leading backslash, which .NET resolves to
      `C:\PhysicalDrive`; it erased the stick and then wrote nothing. Check any
      Windows device path byte for byte: a doubled backslash sent through the
      agent's shell tool can arrive as a single one.
- [ ] Boot test on real hardware; record the xrun count in
      `brain/research/`.
- [x] QEMU boot smoke test. 20 checks, including the live PipeWire quantum
      and rate: a running engine that holds its PID,
      `-rt-` in `uname -r`, no failed units, no serial login prompt, and one
      check per defect found so far. `tools/test-boot-qemu.sh`.
- [x] A way to watch it boot by hand: `tools/run-qemu.sh`, going through the
      real bootloader with the shipped command line.
- [ ] bext OriginationDate/OriginationTime: `WavWriter` leaves both zero
      (`common/audiofile/src/wav_writer.cpp:83-92`). EBU Tech 3285 v2 §2.3:
      10 ASCII characters, year-month-day, and 8 ASCII characters,
      hour-minute-second (separator `-` recommended). Every DAW shows a blank
      creation date. Fill them in `open()`, which runs on the control thread,
      not the audio thread, with a regression test in
      `common/audiofile/tests/test_wav_writer.cpp` first. Then delete the
      `xfail` branch in `tools/bwf-interop.py`, whose format checks already
      run once the fields are non-empty. Found by `tools/test-bwf-interop.sh`;
      see `brain/research/bwf-interop.md`. rt-engineer.
- [ ] Run the boot test in CI. Needs the ISO built first, which takes tens of
      minutes, so it belongs in a separate workflow on a schedule or manual
      dispatch rather than on every push.

## Blocked on hardware the project does not have (OS 1; the UMC1820 is connected at the end)

- [ ] Confirm the Pro Audio profile actually exposes every channel on the
      owner's Behringer UMC, and record the model and channel count in
      `brain/research/multichannel-usb-interfaces.md`.
- [ ] First real latency figure: lowest quantum that holds with zero xruns on
      that interface, on the RT kernel.
- [ ] ADR-0007 IPC figures on the RT kernel, bare metal, with the engine
      recording 18 channels at quantum 128: socket round trip, `StateView`
      read, futex versus eventfd wake. Include the cost of the wake syscall
      on the audio thread, and whether any xrun is attributable to a reader.
      Feeds the Phase 4 plugin ADR. See `brain/research/adr-0007-measurements.md`.
- [ ] OS 1, proxy on the laptop before M8 merges (ADR-0009 update
      2026-10-13). ADR-0009 Unverified 5: worst-case `process()` on
      the target machine with the UMC1820, 18 tracks × 2 voices plus 18
      inputs at quantum 128, pool and plans pre-faulted. rt-engineer.
- [ ] OS 1, proxy on the laptop before M8 merges. ADR-0009 Unverified 7:
      undo-to-audible latency,
      p99 and max, on a real stick while 18 tracks record. rt-engineer.

- [ ] OS 1, needs a physical USB stick: the capture writer wins the device
      over the playback reader (ADR-0009 §4.4, last bullet). Today they are
      separate threads and nothing orders their I/O; measure with 18 tracks
      recording while 18 play back (Unverified 1), then give the writer
      priority (I/O priority class, or the reader yielding while a drain is
      behind) only if the measurement shows a capture overrun or a drain
      delayed by a read. rt-engineer.

- [ ] On the owner's laptop, no interface: the engine records from the
      onboard card after the 60 s wait, the notice on tty1, `--tracks auto`
      gives its channel count (expected 2). Then with the UMC1820: it is
      chosen over onboard, and plugged in mid-session it is noticed and not
      switched to (ADR-0007 M2 note, item 8).

## ADR-0007 follow-ups

- [ ] A pty-driven check (as in `tools/bwf-interop.py drive`) that a quit
      during a failed take exits 1 with the terminal restored. Only the
      `Session::take_failure()` half is unit-tested today.

- [x] RTSan CI job: `clang-22` from trixie main, its own build
      (`-fsanitize=realtime`, which cannot be combined with ASan or TSan),
      `-Wfunction-effects` advisory. `SONORA_RT` expands to
      `[[clang::nonblocking]]`. CI job `rtsan`, `tools/build-rtsan.sh`,
      `-DSONORA_RTSAN=ON`; every ctest runs under it.
- [x] libjack inside the callback under RTSan in CI (`ci/rtsan-jack`):
      `tools/rtsan-jack.sh`, in CI job `rtsan`, runs `process_cb` and
      `xrun_cb` against jackd2 (dummy driver) and PipeWire 1.4
      (pipewire-jack, the shipped pin): `sonora-rtsan-jack-canary` (a
      malloc after process_cb's libjack calls, must be reported), the
      backend conformance suite, and a recorded take. No report:
      `jack_port_get_buffer`, `jack_get_sample_rate` and
      `jack_last_frame_time` are clean on both, so no suppression or
      scope. `xrun_cb` ran (2 xruns, jackd2); under PipeWire no xrun
      could be caused (SIGSTOP of the device or of the engine for 0.2 s:
      xruns=0), so on the audio thread it is unexercised.
- [ ] Conformance `fault_xrun` on jackd2: an xrun caused by stopping an
      upstream client is counted by `xruns()`, but the dummy driver never
      skips a cycle, so the check's "frame_time shows a gap" fails
      (measured 2026-10-14, `ci/rtsan-jack`). Decide whether the gap is
      part of the contract for an xrun the backend's own period did not
      cause; until then `tools/rtsan-jack.sh` causes the xrun in a take,
      not through the suite. rt-engineer.
- [ ] Load-sensitive tests under `ctest -j16` (16-core dev host, RTSan
      build, 2026-10-14): `dsp_triode`/`dsp_triode_generic` (worst hot
      block over the quiet median 6.18 to 8.64, bar 6) and
      `playback_fake` (worst residual 0.354, expected < 1e-6) failed in
      two of five full runs, and pass alone (`playback_fake` 10 of 10).
      CI's 4-core runners have not shown it. Measure under load, or mark
      them RUN_SERIAL. rt-engineer with dsp-engineer.
      `playback_fake` is fixed (PR #71). The 0.354 is the tone's peak
      at the -3 dB bus gain against silence, which is a missed chunk. The
      test ran 8 blocks per 1 ms sleep, about 21 times real time, so in one
      5 ms reader tick it could roll 40 blocks, past the 4800-frame window.
      With 32 busy loops on 16 cores it failed 5 of 40 runs on main, with 1
      to 25 misses. It now waits for a whole reader pass every 8 blocks
      (`DiskReader::passes()`): 0 of 60 under the same load, 0 of 40 idle.
      `dsp_triode` is still open.
- [ ] `-Wfunction-effects` to an error, per module, once clean. Today (clang-22,
      `ci/rtsan`) it reports SONORA_RT functions calling unmarked ones that
      RTSan, at run time, finds clean: `ProcessorBase::apply_event`
      (plugins/native/sdk), `LookaheadLimiter::set_release` and
      `TruePeakGuard::set_release` (limiter), `Bridge::descriptor`
      (format), and libjack's C API (clean
      at run time on jackd2 and pipewire-jack, `tools/rtsan-jack.sh`).
      `ProcessorBase::on_reset` is done (auditor, PR #54): no test reached
      process()'s non-finite-output branch, so `native_sdk` now forces a
      NaN from inside a processor and the harness's `check_reset` runs every
      processor's on_reset from a nonblocking function; both clean under
      RTSan, and the hook is `SONORA_RT`. Mark the
      ours `SONORA_RT` (the hooks are an SDK interface change, so with the
      dsp-engineer's review), then promote the module. rt-engineer.
- [x] In that RTSan job, run `test_playback_mix` and `test_disk_reader`'s
      reader stress test (with its deactivate cycles) at least: they drive
      `Session::process()` and the chunk-slot loads through every playback
      path (fades, crossfades, ramps, misses, locates, swaps, delay lines).
      From the PR #31 RT audit. distro-engineer with rt-engineer.
      PR #41's RT audit adds about ten audio-thread functions checked by
      reading only (FrameClock, PlanExchange::acquire, render_segment,
      FlushDenormals): the job should cover `test_playback_fake` and
      `test_frame_clock` too. All of them run in CI job `rtsan`
      (`ci/rtsan`), clean.
- [x] With M4, the state publisher is paced on a fixed timer. `StateView`
      makes a bounded number of retries and reports a stale flag. Add a
      starvation regression test, and a `static_assert` for every
      shared-memory atomic type. rt-engineer.
- [ ] Double-buffered state block (layout version bump, ADR first). Today
      a publisher preempted mid-section makes every read stale until it runs
      again (PR #53); with two copies and an index flipped after each
      write, readers take the finished copy and never wait on a stopped
      writer. Then the timing tests can hold every tick to "not stale"
      under load, not only to "stale only while the writer was stopped".
      rt-engineer with ui-engineer.
- [ ] Virtual-card QEMU test: `snd-dummy model=rme9652` (26 ordered AUX
      ports) through the real ALSA → PipeWire → JACK path, with the engine
      actually recording. The count is per node. driver-engineer with
      distro-engineer.
- [x] With M3, `drivers/fake` gains two injections from the measurements:
      ports named without `AUX` (`capture_1..N`, what `snd-aloop` gives
      without `audio.position`), and registry ids out of channel order. The
      engine must still connect them in channel order. driver-engineer.
- [ ] Plugin editors under the kiosk compositor. ADR-0010 (accepted) picks
      labwc with the UI on native Wayland, which retires the `xcb` and
      anchor-window variants. Remaining: a real Qt 6 UI and a real LV2/VST3
      X11 editor under ADR-0010's labwc config (placement, override-redirect
      popups, focus back to the UI, Xwayland lazy-start latency). Needed
      before the Phase 4 plugin ADR. researcher.

## ADR-0010 display stack (accepted by the owner 2026-10-13; OS 1, after the Complete DAW)

The M4, M5 and doc items at the end of this section are DAW 1 work and are
not deferred.

- [ ] Stage 1, shipped boot unchanged: package block (`libpam-systemd labwc
      xwayland qt6-wayland` + Qt runtime), `/usr/share/sonora/labwc/`,
      `/etc/pam.d/sonora-ui`, `sonora-compositor`, `sonora-wait-for-display`,
      compositor/UI/fallback units behind `sonora.gui`. QEMU checks: ISO size,
      time to first frame, `kill -9` UI and labwc recover, repeated UI failure
      ends on tty2, renderer chosen on `-vga std`, Alt-Tab/Super-Return reach
      the client. distro-engineer.
- [ ] Stage 1, UI side: `app_id` `sonora-ui`, no fullscreen request, a
      first-frame log line, a documented "unfit display" exit code; later
      `sd_notify` watchdog through `common/platform`. ui-engineer.
- [ ] Stage 2, after M7: `sonora-session.service` becomes
      `sonora-engine.service` + `sonora-tui.service` (tty2); compositor
      enabled by default; smoke test kills labwc and the UI mid-take and
      asserts the engine PID and a growing take file. distro-engineer.
- [ ] Bare metal: GPU reset/hang under the compositor vs the audio callback;
      nouveau on NVIDIA; llvmpipe vs GPU xruns (ADR-0006 items 2 and 3).
      researcher.
- [x] M4: fix the key map comment "Matches what engine/src/main.cpp handles
      today" in `keymaps/default.json` **and** in the copy of that profile
      embedded in `ui/keymap/src/keymap.cpp`, in the same commit, once
      commands are handled in `engine/control`. ui-engineer.
- [x] M4 before M5: remove the illegal edges `engine/app` → `ui/keymap`,
      `ui/tui` and `test_keymap` → `engine/control`, `ui/tui`. M5's check
      would flag them (ADR-0007, "lessons from M1", item 1). rt-engineer
      with ui-engineer.
- [ ] M5: add the §7 ctest labels, one per top-level module directory.
- [ ] With or right after the M1 merge, update docs that name pre-M1 paths
      (`engine/src`, `engine/include`, `engine/tests`):
      `brain/specs/plugin-latency-compensation.md`,
      `brain/research/multichannel-usb-interfaces.md`,
      `brain/research/adr-0007-measurements.md`,
      `brain/research/protools-logic-key-bindings.md`, and the agent
      charters in M6. (`keymaps/README.md` is already done on the M1 branch.) ADRs
      and postmortems keep the paths they were written with.

## ADR-0008 portability

None of these may delay a Phase 1 or Phase 2 item (owner, 2026-10-12).
Order and owners are in ADR-0008 §5.

- [ ] P1, after M2 merges and before M5: `engine/core/src/session.cpp` to
      `std::filesystem`, using only the non-throwing overloads. Add a test:
      a session path that is a file makes `start_recording` refuse.
      rt-engineer.
- [ ] P2, with P1: `test_takes` to `std::filesystem`. Move the full-disk
      case of `test_wav_writer` to `common/audiofile/tests/posix/`,
      registered `if(UNIX)`. rt-engineer.
- [ ] P3, after M2: an `engine/app/include/sonora/process_setup.h` seam,
      covering `lock_memory()`, `install_quit_handlers()` and
      `default_session_root()`, with the implementations under
      `engine/app/src/posix/`. rt-engineer.
- [x] P4, inside M4: `test_keymap` loses termios (those checks move to a
      `ui/tui` test). It removes directories with `std::filesystem`.
      ui-engineer.
- [ ] P5, before or right after `feature/qt-shell` merges:
      `ui/qt/src/measure.cpp` uses `std::chrono` and `std::clock`.
      Per-thread CPU goes through a new `common/platform`
      `thread_cpu_times()`, with posix and unsupported implementations.
      ui-engineer and rt-engineer.
- [ ] P6, with M5: `SONORA_PLATFORM` (linux, neutral, and windows, which
      fails at configure for now) and `SONORA_BACKENDS`. Compiler flags go
      under generator expressions. Add the neutral-module include allowlist,
      a platform-macro check and an OS-path check to M5's script.
      distro-engineer.
- [ ] P7, right after M5 and before M6: a required `portable` CI job on
      `windows-latest` with MSVC, building `SONORA_PLATFORM=neutral` and
      running `ctest`. Record the first 10 run durations in `brain/research/`.
      distro-engineer.
- [ ] P8, inside M6: put the ADR-0008 §1 table into OPERATIONS.md and the
      charters, and propose the rule for CLAUDE.md. The CLAUDE.md edit is
      the owner's.
- [x] P9, inside M7: socket and shared memory behind `common/platform`
      (`ControlChannel`, `SharedRegion`). No socket or mapping code in
      `engine/control` or `ui/client`. rt-engineer and ui-engineer.
- [ ] P10, once `ui/qt` is on main: Qt in the `portable` job, from a cached
      install. distro-engineer.
- [ ] Only when the owner opens a Windows ADR: read the ASIO SDK licence
      from Steinberg's current distribution (it is reported as dual
      proprietary/GPLv3 since 2025-10-15, and that is unverified), and
      record it in `brain/research/` with the GPL-3.0-or-later analysis.
      researcher.

## DAW 1 detail (formerly Phase 2)

- [x] Playback path: read from disk into a ring, mix, feed the output ports.
      Ported onto the M4/M7 control server (ADR-0009 update, 2026-10-13):
      one document, the server's; closed takes replicated as AddSource and
      RecordTake.
- [ ] Plan builds off the control thread. Measured at 18 tracks x 2000
      regions: build_plan() 77 ms p50, 96 ms max (measure_playback, Docker
      on the dev laptop), against the ADR's 10 ms trigger. Every command
      queued behind an edit, transport.stop included, waits that long. Move
      the builder to a worker owned by engine/control, as ADR-0009 §4
      allows, before sessions that size are edited. rt-engineer.
      Left out of the PDC PR (ADR-0012 checklist 3) to keep it reviewable:
      `show()` reports a refused plan synchronously today, and the worker
      changes that contract. The worker takes the PlaybackController's
      PlanContinuity and ChainResolver with it (control-thread state, read
      only by the builder); each build starts from a copy of the playing
      plan's continuity.
- [ ] The stop reply waits on the closed take's plan rebuild:
      `follow_transport()` runs before `reply()` in
      `EngineControlServer::handle()`, so Space's Ok comes after every
      closed WAV header is read, 2xN operations are applied and the plan
      is rebuilt (77-96 ms at 18x2000). Audio is unaffected; the
      acknowledgement is late. Reply first, or fold it into the builder
      worker above. From the PR #41 review (L1). rt-engineer.
- [ ] Prove FrameClock's late-xrun deferral on real servers: under pw-jack
      and jackd2, record `gettid()` in `xrun_cb` and `process_cb`, force
      50 xruns (SIGSTOP the engine for over a period) and check
      `clock_discontinuities()` stays 0. The deferral covers an xrun that
      lands by the next period; one later still would be counted twice.
      From the PR #41 RT audit. rt-engineer.
      Note (`ci/rtsan-jack`, 2026-10-14): on PipeWire 1.4's dummy driver
      in a container, SIGSTOP of the engine for 0.2 s mid-take gave
      xruns=0; jackd2 gave 2 for the same on an upstream client.
- [x] A take on N tracks is one undo step: `add_closed_take()` applies the
      take as one `Compound` labelled "Record" (ADR-0009 update 2026-10-14,
      item 8). A track whose file does not read costs that track, with its
      own notice; a model refusal still puts the audio in the pool and is
      shown as a failed take. PR #75.
- [ ] The end of a fader gesture written into the document as `SetMix`.
      Today a `ParameterChange` moves the Track atomic only, so the session
      file and every replica keep the old gain, and the next mix-changing
      update (another track's `SetMix`, a load) snaps the fader back.
      ui-engineer with rt-engineer.
- [ ] `transport.locate` with the frame in `argument` (a click on the ruler),
      and state block layout v2 carrying "Locating...", the monitor bus
      peaks and the playback miss count. Contract PRs of their own (ADR-0009
      update, item 4, "Not in this port"); the bus meters return in
      sonora-tui and sonora-ui with them. rt-engineer with ui-engineer.
- [ ] Input monitoring with latency compensation measured by a loopback test.
- [ ] Transport locate without dropping a block.
- [x] Flake explained: the loopback PLAYBACK check's swapped blocks were a
      period PipeWire's dummy driver skipped without an xrun, which the
      self-linked input reads as two blocks swapped; the engine's output was
      continuous. The skip is now a clock step, and only then may a block
      null a period early or late. Postmortem
      2026-10-13-loopback-swap-skipped-period.
- [ ] Locate latency as ADR-0009 §4.4 asks: p99 and max per session in
      diagnostics, and "Locating…" on screen past 100 ms.
      `PlaybackController` keeps only the last and the max today. rt-engineer
      with ui-engineer.
- [ ] Show `PlanReport::over_half_pool` when a plan is built (ADR-0009 §4.3:
      a plan whose peak demand exceeds half the pool is flagged before it
      plays), and the "edit not yet audible: playback cache full" counter
      from `DiskReader::cache_full()`. The builder computes both; nothing
      shows them yet. ui-engineer.
- [x] A short ramp when a track's monitor source changes (input to playback
      and back, Auto at record start and stop). Every change of source now
      ramps over `PlaybackConfig::monitor_ramp_frames` (64) from the frame it
      takes effect (PR #81, ctest `punch_points`). Not
      ramped yet: Off to playback while rolling, which is a mute-like step
      from silence.
- [ ] Timeline data model: regions over sources, with an undo stack.
- [ ] Session file format spec, then the reader and writer. Specs written
      (`brain/specs/session-format.md`, `brain/specs/timeline-model.md`,
      ADR-0009 accepted 2026-10-13); reader and writer not started.
- [ ] `common/json`: strict JSON with integer-exact numbers, line and key
      path in errors, size and depth caps (ADR-0009 §5). `common/model`
      uses it; `ui/keymap` moves onto it in its own PR. rt-engineer, with
      ui-engineer review.
- [ ] Bug: `ui/keymap`'s JSON parser reads numbers with `std::strtod`
      (`ui/keymap/src/keymap.cpp`, `number()`), which honours the C locale.
      Under `de_DE` the decimal separator is `,`, so `0.5` parses as `0`.
      Fix with `std::from_chars`; regression test that sets a comma locale
      first. ui-engineer.
- [x] Close a take while rolling with **in-band boundary records**
      (`brain/specs/timeline-model.md`, "Closing a take while rolling"):
      audio thread → SPSC ring → disk thread splits at exactly the frame;
      the capture ring is never reset; the next take's files opened ahead;
      the disk thread reports "take closed, length" before `RecordTake`.
      Capture start/stop and the auto-monitor switch are sample-accurate
      inside a block, with a ramp at A and B. This **replaces the take-epoch
      handshake** in "Known gaps": with no ring reset there is no race. The
      race's regression test is still written first. rt-engineer, audited
      by rt-safety-auditor.
      Landed on `feature/record-while-rolling` (2026-10-14), except the
      sample-accurate parts: windows open and close on block boundaries, and
      the auto-monitor switch has no ramp yet. Both go with the punch points
      below.
- [ ] Punch points inside a block: capture opens and closes at a frame the
      control thread sets ahead (Pro Tools' punch range, Logic's autopunch),
      sample-accurate, the monitor switched there with a ramp; files opened
      when the punch is armed. Then a `transport.punch` command in the
      `contracts/api` catalogue for punching in and out on the fly
      (`Session::start_recording()` while rolling, `Session::punch_out()`),
      and its vendor bindings in `keymaps/`. rt-engineer, contract owner for
      the catalogue.
      Engine/core half landed (PR #81):
      `Session::start_punch(take, PunchRange)` opens the files when the punch
      is armed; the audio thread opens and closes each capture window at
      `capture_from`/`capture_to` inside a block (offsets 0, 1, nframes - 1
      tested) and closes the file in band with the transport still Recording;
      an armed Auto track crossfades playback to input centred on A and back
      on B with the punch's length and shape (ctest `punch_points`).
      Remaining: (a) the caller. Nothing calls `start_punch` yet; engine/control
      computes `[A - H, B + H)` from the session's punch range and
      `punch_crossfade`. (b) Placement: `add_closed_take` places a closed take
      at its TimeReference for its whole length, so a punch take would cover
      its handles; it must place `[A, B)` with the handles as source offset
      and the two crossfades (timeline-model.md, "Punch in" step 3), on top
      of #75's compounds. (c) `transport.punch` in the catalogue, under
      ui-engineer review (ADR-0007 §6). (d) The `failed_files_` item below.
      Before it lands (PR #60, review 4): `Session::failed_files_` lasts
      until `stop()`, so after take K fails, `take_failure()` stays
      non-empty through a clean take K + 1 started while rolling.
      `engine_control_server.cpp` (the `TakeFailed` block in the transport
      follower) would then post "take K + 1 failed" naming K's file, and the
      health flag would never clear, though ADR-0007 says the next clean take
      clears both. Key the failures by take, or clear them when a take's
      close is collected. Unreachable today: `transport.record` during a
      take does nothing.
- [ ] Periodic `fdatasync` of every open take file from the disk thread,
      through `common/platform`, with the loss window stated in the UI and
      in `brain/specs/session-format.md` ("Crash recovery"). Measure its cost
      on a stick at 18 tracks first; it must not cause an overrun.
      rt-engineer.
- [ ] Disk reader thread and source chunk pool with per-plan chunk-slot
      arrays; plans and locates published only once their window is
      resident; one pending plan at most; exhaustion never evicts the
      playing plan (ADR-0009 §4). Comes with the ThreadSanitizer stress test
      of the retirement protocol (§4.1). rt-engineer, gated by
      rt-safety-auditor.
- [ ] Playback to `AudioBlock::outputs` through the monitor bus. The
      exit-criterion ctest needs `drivers/fake` with captured outputs
      (`FakeBackend::read_output()`, from M3). rt-engineer.
- [ ] Measure, before the reader is written: mixed record+playback
      throughput and worst-case read latency on a physical stick's
      persistence partition (18 in, 18 out, 8 k and 16 k frame chunks,
      coalesced reads). researcher; ADR-0009 Unverified 1.
- [ ] Measure before M8, no hardware needed: pool demand of a 2,000-region
      comp at the placeholder chunk size (ADR-0009 Unverified 6); undo
      memory for single and bulk edits on 18 × 2,000 regions against the
      64 MiB budget (Unverified 3). rt-engineer. The hardware ones are under
      "Blocked on hardware".
- [ ] Measure: `rename` plus directory `fsync` durability through the
      persistence overlay, 200 killed QEMU saves. researcher; ADR-0009
      Unverified 2.

## Known gaps, not yet scheduled

- RT-path data race, documented in `engine/core/src/session.cpp` above the
  ring reset in `start_recording()`: an audio callback that loaded Recording
  for the previous take and was descheduled across `stop()` and the next
  record command can write to the ring while it is being reset, putting the
  old take's samples at the head of the new file. Needs an RT stall of about
  100 ms to hit. Fix with a take-epoch handshake with the callback; write the
  regression test first. rt-engineer, audited by rt-safety-auditor.
  **Superseded by ADR-0009:** the fix is now the in-band boundary records
  in Phase 2 ("Close a take while rolling"), which remove the ring reset.
  **Fixed on `feature/record-while-rolling`:** the ring is never reset, and
  `test_take_boundaries` reproduces the race on the old code (an insert
  runner's `begin()` stalls a callback after it has loaded the take).

- RF64 for files over 4 GB. The 32-bit RIFF size field overflows at about
  8 hours of 24-bit mono at 48 kHz. See `brain/specs/bwf-broadcast-wave.md`.
  Until then `WavWriter` stops at the limit instead of wrapping: the file
  ends at the last whole frame that fits, and the take is reported failed
  ("take reached the 4 GB WAV limit") when it stops.
- Session origin. Every session's first take has TimeReference 0, because
  the playhead starts at 0. Ardour 8 treats 0 as "no timestamp" and imports
  it at the edit point (`gtk2_ardour/editor_audio_import.cc:989`, tag 8.12).
  A non-zero session start would avoid that, but it is a timeline decision
  (ADR), not a writer fix. `brain/research/bwf-interop.md`, finding 2.
- A `stop()` whose wait for the callback in flight times out (1 s, a hung
  backend) still returns true. The block that callback records late shows
  only on stderr and in `Session::dropped_samples()`, which the UI does not
  surface (PR #60, review 5). Acceptable for a hung backend, but the
  operator should see it: a Notice, or the health flag.
- `DiskThread` polls every 10 ms. Correct, but a condition variable signalled
  by the callback would be better once there is a reason to care. It must not
  make the callback block.
- The disk thread writes with buffered `stdio`. Direct I/O with aligned buffers
  avoids double-buffering through the page cache; worth measuring before
  changing.
- No RT priority is requested for the audio thread yet; PipeWire sets it for
  its own client thread, which is enough until we own threads.
- `KeyMap::terminal_chords()` assumes xterm's modifier encoding. On the Linux
  console (tty1, where the appliance runs) Alt+Fn and Alt+Left/Right switch
  virtual terminals, Shift+PgUp/PgDn scroll the console, and Ctrl/Shift+arrow
  send no modifier parameter at all, so those chords never reach the engine
  but are counted as sendable by `ensure_essentials()` and listed in the
  footer. No shipped essential depends on one today; a profile that put its
  only arm, record, stop or quit key there would pass the check and not work.
  Fix by deriving the device set from the console's keymap (`dumpkeys`), or
  by a tty1-specific table.

## ADR-0011 native plugin suite (proposed 2026-10-13)

Stages N0–N7 are ADR-0011 §6. Nothing here goes into `engine/` except the
meters (N1), which the rt-engineer writes. Owner rulings that gate items are
ADR-0011 "For the owner".

- [ ] N0: `common/dsp` skeleton (neutral, `arch/generic` + `arch/x86_64`
      for flush-to-zero), `plugins/native/sdk` (`Processor`, parameter table,
      descriptor and preset codecs on `common/json`), and the new edges in
      M5's check. dsp-engineer.
- [ ] N0: `sonora-dsp-measure` harness and golden-file infrastructure:
      response, THD, step timing, aliasing, latency equals reported,
      denormal, non-finite, zipper; RTSan job runs it. dsp-engineer.
- [ ] N0: BS.1770 loudness and true-peak in `common/dsp`, checked against
      EBU Tech 3341/3342 cases (redistribution of the test files to be
      checked first). dsp-engineer.
- [ ] N0: EQ, gate, compressor, limiter, delay, chorus as `Processor`s,
      meeting their `quality.json`. CPU figures on the owner's laptop
      recorded in `brain/research/` (ADR-0011 Unverified 4). dsp-engineer.
- [ ] Still missing after N0 (PR #34 review, 2026-10-13): ~~the RTSan job
      that runs the harness and the golden tests~~ (done in `ci/rtsan`:
      native_* and dsp_* run under RTSan in CI job `rtsan`), and the MSVC `portable` job that runs the
      golden tests under MSVC (ADR-0011 §3.4, ADR-0008 P7 above). Until
      both exist, "RTSan-clean" and "passes under MSVC" are claims nobody
      has checked for `common/dsp` and `plugins/native`. distro-engineer
      with dsp-engineer.
- [ ] Research, before N1: licences of DPF (pinned commit), the VST3 SDK
      (MIT since 3.8, reported), ONNX Runtime; ONNX Runtime on trixie and
      its installed size (ADR-0011 Unverified 1, 3). researcher; feeds
      `brain/research/native-plugin-suite.md`.
- [ ] N1, after `feature/plugin-host-lv2` merges: `plugins/native/format`
      on DPF, LV2 and VST3; `lv2lint` and Steinberg `validator` in CI; the
      DPF-VST3 checkpoint on the EQ (Unverified 2). dsp-engineer,
      plugin-engineer reviews.
- [x] N1 follow-up (PR #40 review, code L2): render through a bundle the
      paths the host fixture never reaches. Done in `feature/dsp-n1-io`:
      a parameter moved on the ring mid-render (`automation-48000`, every
      processor, both formats, bit-exact), VST3's one-block-late delivery
      at a nonzero offset (`native_format_late_vst3_*`), and the EQ's mode
      and the limiter's look-ahead set at load through a host that
      re-activates (`reconfigure-48000`, host `--reactivate`).
- [ ] N1, with M9: re-activating a plugin while it runs, for a
      latency-changing value moved during playback, then re-planning PDC.
      Needs the HostControl channel: the host pauses its processing
      thread, re-activates on its main thread (`Processor::reactivate`
      exists), publishes the new latency. Only the load-time case
      (`--reactivate`) is tested now. plugin-engineer + rt-engineer.
- [ ] N1, with M9: how the engine routes a key to a plugin's side-chain.
      The host takes it today as the last N channels of the region
      (`sonora-plugin-host --sidechain N`, the header unchanged);
      `contracts/plugin` should say so, or carry key planes of its own,
      before the engine writes against it. plugin-engineer + rt-engineer.
- [ ] N1 follow-up (PR #40 RT audit, low): DPF's `lv2_set_options` calls
      `setBufferSize(..., true)`, which deactivates and re-activates, and so
      re-prepares and allocates, on whatever thread the host calls
      `options:set` from. Check the LV2 options interface's threading class
      and what Ardour, Carla and our host do. If a host can call it beside
      `run()`, defer the change to the next `activate()`. dsp-engineer.
- [ ] N1, with M9: plugin-parameter target on `ParameterChange` and the
      read-only telemetry region in `contracts/plugin`. plugin-engineer +
      rt-engineer + ui-engineer.
- [ ] N1: descriptor-driven Qt editor panels in `sonora-ui`. ui-engineer.
- [ ] N1: engine track and master meters on `common/dsp` (peak, loudness,
      true-peak), gated by rt-safety-auditor. rt-engineer.
- [x] N2 slice 1, `feature/dsp-n2`: the FDN reverb end to end.
      `common/dsp` `fdn.h` (16 lines, Hadamard, three-band T60 per line
      after Jot and Chaigne, lossless freeze), `plugins/native/reverb`
      against its `quality.json` (T30 per octave band, ISO 3382-1, within
      5 % of the set decay; onset on the pre-delay to the sample), LV2 and
      VST3 bundles, stereo and mono, through N1's format layer.
      Measurements: `brain/research/native-reverb-n2.md`. dsp-engineer.
- [x] N2 slice 2, `feature/dsp-n2-tube`: ngspice in the plugins image and
      the SPICE reference harness (`measure/spice.h`: netlists generated
      from the model's own values, committed, re-run by `spice_triode_*`);
      the triode stage in `common/dsp` (`triode.h`: Dempwolf and Koren laws,
      grid current, interelectrode capacitances, MNA/DK Newton solve); the
      tube drive processor on it (2x/4x/8x, LV2 and VST3, stereo and mono).
      Measurements: `brain/research/native-tube-n2.md`. dsp-engineer.
- [ ] N2 slice 3: transformer (core saturation and hysteresis, LF first)
      as an output stage of the tube drive, with the supply sag an output
      stage draws (slice 2 has one preamp stage, which draws too little
      for sag to matter). dsp-engineer.
- [ ] N2 tube follow-ups (slice 2): (a) CPU: eco (2x) meets class B at
      44-48 us p99.9 per q128 stereo block on programme and full-drive
      worst cases (Docker Desktop, i7-11800H); standard (4x) is 72-110 us
      and high (8x) 137-155 us, over it, beside eco as ADR-0011 §3.5
      allows. Next candidates, measured on the reference machine first:
      the DK method's precomputed solution table over the two port
      voltages (Yeh 2010) against accuracy to ngspice, and fewer passes on
      regime changes (`brain/research/native-tube-n2.md`). (e) Before the
      first arm64 build: flush-to-zero in `common/dsp/src/arch/aarch64/`
      (FPCR.FZ); the generic arch does not flush, and the oversampler's
      and delays' histories reach subnormals after ~16 s of silence.
      (b) The triode laws against published 12AX7 plate
      curves or the owner's tubes (slice 2 checks the solver against SPICE
      on the same laws, not the laws against a tube). (c) The bias
      (cathode resistor) and a second stage as parameters, each with its
      SPICE cases. (d) Golden files under Clang and MSVC once those jobs
      exist. dsp-engineer.
- [ ] N2 slice 4: tape (Jiles-Atherton hysteresis, head bump, gap loss,
      NAB and IEC record and playback EQ, wow and flutter). dsp-engineer.
- [ ] N2 slice 5: modelled noise and hum in `common/dsp` (seeded, published
      spectra and levels, digitally silent at zero, per-instance level for
      the editor: ADR-0011 §3.3), wired into the tube, transformer and tape
      processors. Default on or off is the owner's (item 7). dsp-engineer.
- [x] N2 slice 6a, `feature/dsp-n2-dynamics`: the optical levelling
      amplifier. `common/dsp` `opto.h` (the published attenuator and side
      chain, the EL brightness law, CdS photoconductor kinetics with two
      trap levels), its SPICE reference (`measure/spice_opto.h`, ctest
      `spice_opto_0..5`), "Sonora Opto" (eco 1x zero latency, standard 4x,
      high 8x; LV2 and VST3, stereo and mono, key input). Measurements:
      `brain/research/native-opto-n2.md`. dsp-engineer.
- [ ] N2 slice 6b, `feature/dsp-n2-fet` (PR #74), landed; open on (e) of
      the follow-ups below (eco's CPU): the FET peak limiter ("Sonora
      FET") from the reissue manual's published circuit (Figs. 3 and 5):
      the JFET as the divider's bottom leg with the LN gate, the feedback
      side chain, ratio and bias strings, rectifier, attack (R54 || C27,
      R55) and release (R56, R57) networks, C9, the stereo link joining the
      C22 nodes; [SH] with the National 2N5457 model; a joint per-sample
      Newton solve of the drain and the rectifier (`common/dsp` `fet.h`).
      SPICE reference through `measure/spice_fet.h` (ngspice's own NJF and
      diode), `plugins/native/fet` with LV2 and VST3, stereo and mono, with
      the key input. Measurements: `brain/research/native-fet-n2.md`.
      dsp-engineer.
- [ ] N2 FET follow-ups (slice 6b): (a) The all-button mode: under our
      reading of Fig. 5's switch it gives about 3.6:1 from a threshold
      12 dB below 4:1's, where the manual says "between 12:1 and 20:1".
      Trace the switch's contacts on the original service schematic or a
      unit, and model what it shows. (b) The release's slow stage: C9
      (0.22 uF behind R6 + R7) holds half the gate's excursion for about a
      second, so from 11 dB at the fastest release the reduction takes
      0.36 s to halve. Confirm C9's value and the R6 arm's DC return on
      the service schematic or a unit before anything changes. (c) The
      ratios come out 10-24 % under the labels with the National 2N5457
      model; measure the bias string's voltages and the side chain's gain
      on a unit, and the FET in it. (d) The class-A output stage and its
      transformer, the input transformer and the preamplifier's own
      colour, after the transformer slice (3). (e) CPU, before the FET
      enters the default insert list: margin under class A. The first
      round (the libm calls out of the solve, bit-exact) took eco from
      20.5 to 19.66 us p99.9 per q128 stereo block on the golden
      configuration (quiet host at the reference kernel's speed,
      i7-11800H; 16.6 us on the hot programme): inside class A by 1.7 %,
      which is not margin. Measured and not kept, with figures in
      `brain/research/native-fet-n2.md` (CPU): an error-based stop, starts
      from the linear network, a chord second iteration, the two channels
      in SSE2 lanes (same bits, 6-14 % slower), an inline table-driven
      exp, looser stop tolerances. The p99.9 is blocks of two iterations
      around slow zero crossings. Next candidates: the rectifier solved in
      its current rather than its junction voltage; a predictor across
      the zero crossings. Then add the FET's eco to the nightly gate and
      record `cpu.p999_kernels`; standard (80 us) and high (147 us) ship
      over the class only beside an eco that meets it (§3.5).
      (f) Eco under-reads high-frequency peaks (up to 2.5 dB at fs/4) and
      aliases heavily at the fastest settings (-34 dBc at 48 kHz): an
      inter-sample peak estimate for eco's rectifier, if a low-latency eco
      is wanted. (g) The slowest attack is a third of the manual's 800 us
      on the suite's step: settle how the manual measured it. (h) The
      Aveiro thesis and Gerat, Eichas and Zolzer (AES 143) for
      validation figures, not reached here. (i) A component-level
      netlist of Figs. 3 and 5 for the SPICE reference (the transistors
      of the preamplifier and side chain, the bias string, the switch,
      the gate network with C3 and C6): today ngspice runs the same
      reduced circuit as the model, with the gate, side chain, bias taps
      and rest point as behavioural sources, so it checks the solver and
      the device laws, not that reduction. dsp-engineer.
- [ ] N2 opto follow-ups (slice 6a): (a) CPU: standard (4x) is 42-47 us
      p99.9 per q128 stereo block over class A (20 us), beside eco at
      9 us that meets it; candidates, measured on the reference machine
      first: the two channels in SIMD lanes (as the triode pair), a
      tabulated EL law (1e-4 relative is enough, under 0.001 dB), the
      side chain at half the audio path's rate. (b) The SignalTrain LA-2A
      recordings (Zenodo 3824876, CC BY 4.0, 20.7 GB): the gain-reduction
      envelope at the dataset's settings against the model, and the
      cell's constants refitted if it disagrees; the researcher confirms
      the terms cover the refit first. (c) The original amplifier (12AX7
      voltage amplifier, 12BH7 cathode follower, transformers) on slice
      2's triode, with its SPICE cases, as an option (about class B).
      (d) Unverified readings to settle on hardware: the limit/compress
      switch (R7 shorted in compress), and the side chain not loading the
      tap. (e) Record `cpu.p999_kernels` for the opto's eco from two
      nightly runs, so the calibrated gate also holds its regression.
      dsp-engineer.
- [ ] Nightly CPU gate (PR #55 follow-up): an inconclusive calibrated run
      (exit 3) is a warning and passes the night, so a runner that drifts
      every night would hide a regression indefinitely. Retry the
      calibrated step once on exit 3, and fail the workflow after N
      consecutive inconclusive nights (the count kept as a workflow
      artifact or cache entry), N set from the inconclusive rate the
      nightly has seen. dsp-engineer.
- [x] Wall-clock bars under the sanitizers (found on the opto PR, not
      caused by it; done in #73: Suite::check_time_le, SONORA_SANITIZED): the tube suite's `hot_input` holds every quality's
      worst block to the 2.67 ms period (`check_le`, 2667 us) in every
      build, ASan/UBSan included, and with or without SONORA_DSP_QUICK.
      It passes on the GitHub runners (CI `sanitizers`, nightly `asan`),
      but an ASan/UBSan ctest at -j16 on the owner's laptop (Docker
      Desktop, i7-11800H, 2026-10-14) failed high (8x) at 4.5-4.9 ms per
      block. The opto suite has the same check (worst 0.1 ms in Release;
      it passed in that ASan run). A sanitizer slows code by an unknown
      factor, so the bar means nothing there: hold it in the uninstrumented
      builds only and report the figure under a sanitizer (a CMake define
      from the sanitizer flags, not a platform `#ifdef`). The bound on the
      output, the point of the regression test, stays held everywhere.
      dsp-engineer.
- [ ] N2 slice 7: the mix bus processor (console-bus colour, bus
      compressor) on slices 2 to 6. dsp-engineer.
- [ ] N2 slice 8: the tuner's DSP (YIN-family detection, PSOLA or phase
      vocoder shifting, key and scale as parameters), evaluated on VocalSet
      and vocadito (CC BY 4.0). dsp-engineer.
- [ ] N2 reverb follow-ups (slice 1 review): slow modulation of the line
      lengths against metallic ringing at long decays, measured as a
      spectral-flatness or modal-ringing figure first; an early-reflection
      stage; decay calibration and listening against the Arni and Motus
      room responses (CC BY 4.0); the 63 Hz band, reported but not held to
      the bar below a bandwidth-time product of 100, held by averaging more
      responses or a longer render; the shortest decays' residue (decay
      0.2 s, high x0.1: a -224 dB second-order shelf whose pole near DC
      leaves a tail 80 dB down falling at about 60 dB/s,
      `brain/research/native-reverb-n2.md`), by capping the per-line shelf
      depth or a higher-order absorption design. dsp-engineer.
- [ ] Before the first capture session: class-E cost of NAM-class
      architectures plus a 200 ms cabinet IR at q128 (Unverified 9).
      dsp-engineer.
- [ ] N3: `common/dsp/neural` kernels, `.nam` reader, partitioned
      convolution, amp and cab processors; equivalence test against the NAM
      MIT reference implementation; first captures and IRs of owned or
      rented hardware with capture cards. dsp-engineer; hardware is the
      owner's (item 6).
- [ ] N4: mastering console as one processor; `sonora-analyze` with a
      rule-based mastering-suggestion pass. dsp-engineer.
- [ ] Research, before N5: a source-separation model whose weights we may
      ship (Unverified 10); accuracy of reverb/delay estimation from a
      reference (Unverified 11). researcher.
- [ ] N5: mix assistant plan machinery (plans as `common/model`
      operations, diff preview, one compound undo); first feature
      tempo-synced reverb and delay with reference estimation and the
      excess check; then gain staging, buses, parallel chains, mix bus.
      dsp-engineer with ui-engineer.
- [ ] N6: ONNX Runtime in `sonora-analyze`, model cards and manifest; key
      detection, timbre matching, artist-tone matching, tuner following the
      session key. dsp-engineer; researcher clears each card.
- [ ] N7, only after the owner's rulings (item 8): `sonora-assistant`, the
      optional Claude layer over analysis features and session state, never
      audio; graceful fallback to native suggestions. ui-engineer.
