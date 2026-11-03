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

- [x] `transport.locate` command to a frame, so marker recall moves the playhead (done in #123).
1. [ ] M2 backend contract, `contract/backend` (in review).
2. [x] M3 `drivers/fake` and the conformance suite, with the two port-order
       injections ("ADR-0007 follow-ups"). `feature/fake-backend`.
3. [ ] Playback: disk reader, chunk pool, residency-gated plan swap, output
       through the fixed 2-channel monitor bus (DAW 1 detail, below).
4. [ ] Record while rolling with in-band take boundaries; punch, loop,
       locate, pre-roll; input monitoring. In-band boundaries, record
       while rolling and punch in/out on the fly at block boundaries:
       `feature/record-while-rolling`. Punch points inside a block, engine
       side: PR #81. Their caller and placement (PR #91):
       record with the session's punch range set punches over
       [A − H, B + H), and the take goes in over [A, B) with a crossfade at
       each punch point that has the handle, one undo step.
       - [x] `transport.punch`, punch in and out on the fly: during a
         take, it punches out with the transport rolling on; otherwise it
         records as `transport.record` does. With no punch range that is at
         the playhead; with one (PUNCH lit, Logic's Autopunch) the take
         stays inside it: before A it punches in at A, inside [A, B) at the
         playhead, out at B either way, and at or past B it is refused by
         name (ui-engineer review on #97). In the start-a-take group of
         `KeyMap::essential_commands()`, not the stop group. In the catalogue, `install_session_commands()` and
         `FakeEngine`; Logic's Record/Record Toggle (keypad `*`) bound to it.
         Pro Tools has no key for it outside QuickPunch mode, which is
         `transport.toggle_quickpunch`, not in the catalogue. A failed take
         no longer follows into the next one punched in (`failed_files_`
         keyed by take). Needs the ui-engineer's review in the PR thread
         (ADR-0007 §6, additive).
       - [x] Punch follow-ups (`feature/punch-followups`): past B + H the
         take ends and the transport drops to Rolling, as Pro Tools' record
         button does at the punch-out, for `transport.record` with a range
         and `transport.punch` (`Session::follow_punch_out()`, no contract
         change: the state block's Rolling says it). A punch whose range is
         already behind its first block (the #91 race past B + H) closes
         its files at once, empty, and any punch that ends with nothing
         captured posts a Notice saying why. The engine spells positions in
         refusals and notices one way, "frame N" (`sonora/frame_text.h`,
         FakeEngine included). Showing them in the operator's counter
         format waits for structured positions (protocol 8, after #100);
         a free-text rewrite in the UI was dropped in review, since it would
         corrupt paths and plugin text.
       - [x] The take's bext Description (`feature/punch-bext-description`):
         `sonora take=<n> track=<id> punch=<A>-<B> session=<name>`, the
         punch only for a punch, written when each file opens, so a take
         killed mid-punch still says where it goes. The name is last and
         the rest of the field (it may hold spaces and `=`). Track id and
         name are the document's, set by `PlaybackController::show()`; the
         id is left out when the engine has none. Reading it back at
         recovery is `engine.record.orphan_offer`.
       Loop record (ADR-0016): with the document's loop range set, a take
         records from A less the pre-roll through every wrap into one file
         per track, each wrap marked in band and kept in `<file>.wraps`;
         its passes go in as one `RecordLoopTake` per track, on playlists
         of their own, the last complete one on the active playlist.
       Loop playback (`t1-loop-playback`): with the loop range set, a roll
         wraps from B to A at the sample; a new range goes to the audio
         thread once its start is resident, the old one held until then
         (ADR-0016, update "loop playback when not recording").
       Loop on/off (`t1-toggle-loop`): `transport.toggle_loop` (Ctrl+Shift+L,
         keypad 4, Logic's C) flips `transport.loop_playback` (session
         format 9, off by default, saved, not undoable; protocol 17), which
         gates the roll's loop and loop record; turned on with the Edit
         selection, the selection becomes the range (ADR-0016, update "the
         loop on switch"). The Transport window's Loop button lights from it.
       Loop record mode (`t1-toggle-loop-record`): `transport.toggle_loop_record`
         (Pro Tools' Option+L) flips `transport.loop_record` (session format
         10, v9 opens it as loop playback was; protocol 23), the only gate
         for a loop take; loop playback gates the roll alone. Logic's C is
         `transport.toggle_cycle`, both modes together over the kept range,
         the selection becoming it when none is set (refused with neither);
         Cmd+U is `transport.cycle_selection` (ADR-0016, update "Logic's
         cycle area"). A loop take refuses any change to the range
         or either mode, from a command or a SetSettings. Transport window:
         Loop and Loop Rec, or one Cycle button (ADR-0016, update "loop
         record as a record mode").
       - [ ] engine.transport.loop: with loop playback on, a new Edit
         selection (PT profile) becomes the loop range, as Pro Tools' loop
         follows the selection. Today the range moves only when a mode turns
         on, and play A-B over a different kept range locates to A less the
         pre-roll, stops at B plus the post-roll and wraps at the kept range's
         B in between: the selection's play and the kept loop disagree. Needs
         a choice of which wins, per profile (Logic plays the cycle area).
       Remain: pre-roll for a plain take. Input monitoring
         with latency compensation: ADR-0019, `t1-input-monitor`.
       - [x] Loop record: a track that opens late and loses every later
         wrap still numbers its passes from P1; the Open record needs the
         take's wrap count. The Open record carries it; the disk thread
         holds those wraps' places as WrapSidecar::kBeforeFile (wrap list
         version 2, version 1 reads as 2), never placed by loop length
         (`t1-late-open-wraps`).
       - [ ] Loop record: a late open past `loop_pass_cap` labels its tail
         pass P(cap+1); drop passes past the cap using `wraps_past_cap`
         (`disk_thread.cpp:204` with `closed_take.cpp:389`); practically
         unreachable.
       - [ ] `session-format.md`: note that engines before wrap list v2
         refuse it, and place a loop take's passes by loop length.
       - [x] Loop stall test: hold the drain for the full
         `disk_stall_seconds` (0.5 s), not 300 ms.
       - [ ] Loop stall test (#196 review): hold the disk-thread drain through
         a test hook for `disk_stall_seconds` instead of relying on blocks
         outrunning the 10 ms wakeup; also assert the boundary ring capacity
         is >= tracks*ceil(disk_stall_seconds*sample_rate/max_block_frames)
         so power-of-two rounding cannot hide an undersized formula.
       - [ ] Optional: reserve the extra reader window only during loop
         takes (the pool is doubled now).
       For the Transport window (Track 3 item 3, `feature/ui-transport`),
       which draws these unavailable and says why until they exist:
       - handlers for `transport.rewind`, `transport.fast_forward` and
         `transport.go_to_end` (`transport.toggle_loop`: `t1-toggle-loop`);
       - a locate command taking a frame (`transport.locate`, argument),
         for counter entry and memory locations;
       - [x] pre/post-roll from the session's `transport.pre_roll`/`post_roll`
         (`t1-preroll`, timeline-model.md "Pre-roll and post-roll"): a
         punch started stopped locates to A − pre-roll; `transport.play` and
         `transport.toggle_play` take an optional `A-B` range; the audio
         thread stops the roll at B + post-roll, at the sample, unless a
         playback loop wraps first (`t1-postroll-loop`); the Qt UI sends its
         selection as that argument to an engine announcing
         `transport.play.range` in Hello. Remains: pre-roll for a plain take;
       - [x] the click and count-off from the tempo map (`engine.core.click`,
         `t1-click`, ADR-0020): beats from the plan's click segments on the
         map's frames, accented on beat 1, after the bus gain on the
         monitor outputs, never in a take; `transport.count_off_bars`
         (session format 8, default 2) of count-off before a take started
         stopped, the playhead and the take held; `transport.toggle_click`
         and `transport.toggle_count_off`, `transport_flags` in the state
         block (layout 4), protocol 15; the Transport window's two buttons.
         Remains: the switches saved with the session, a click level and an
         output of its own, Pro Tools' "only during record".
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
       the `fdatasync` cost on a stick (ADR-0009 Unverified 8, OS 1);
       measure `find_orphan_takes()`, which opens every `.wav` in the shared
       engine folder on the control thread at start and at each Load, on a
       large folder, and move it off that thread if it delays heartbeats
       (PR #132 review, non-blocking 4).
       Also open: placing left takes (the "Leave" answer of the orphan
       offer) later, from Audio Files / Clip List (PR #132 review).
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
       - [ ] FakeEngine bus peaks from its track signal, so the conformance
         suite can check the bus meter's values.
10. [ ] Engine commands the shipped key maps bind and no engine runs yet
        (Track 3 item 4; the window says "not implemented" for each):
        `transport.go_to_end`, `transport.rewind`, `transport.fast_forward`,
        `transport.play_half_speed`. Wired in
        the interface against `FakeEngine::Config::planned_commands`
        (test_ui_keymaps); each needs its handler in
        `install_session_commands()` and in `FakeEngine::engine_commands()`
        together, and then leaves `planned_commands`. `edit.cut`/`copy`/
        `paste`/`duplicate` run in the interface as compounds (Track 3 item
        1, PR #86), and `edit.repeat` and `edit.select_all` too
        (`t3f-repeat`, `t3-multitrack-clipboard`);
        `heal_separation`/`consolidate` are still to build.
        - [x] `track.toggle_input_monitor` (Pro Tools' TrackInput) runs in
          the interface, as `track.toggle_mute` does: Input mode is the
          document's `TrackMix::monitor`, already saved with the session
          (no format or protocol change), and the engine follows it through
          `apply_monitoring`. Both engines publish `kTrackInputMonitor`;
          conformance "input monitor mode". "I" buttons in the Edit
          window's headers and the Mix window's strips
          (`t1-track-input-monitor`).
11. [ ] Session-folder follow-ups (ADR-0018 step 4; #150 review items 1-4
        done in #151, `t1-folder-followups`):
        - [x] ActiveFolder::enter() releases the old folder's flock before
          remove_unreferenced(); delete while still holding it, and unlink
          only the *.state(.tmp) entries holds_no_data() checked (#151
          review N1; `t1-folder-lock-order`)
        - [ ] ADR-0018 §1 step 6 still says the cleanup "can never delete a
          file"; update it for the states-only removal of #151 (#151 review
          N2)
        - [ ] A Save into a subfolder of the untitled folder writes a session
          file whose relative take paths do not resolve; refuse it or route
          it to Save As (§5) (#151 review N3)
        - [ ] main.cpp exit message and the new-session reuse check still use
          ActiveFolder::fresh(); align them with the states-only disposal so
          the journal is right and a New does not become "Untitled 2" (#151
          review N4)
        - [ ] FakeEngine::Config::sessions_root is never set by a rig; cover
          its folder-Load sessions-root branch in a test (#151 review N5)
- [ ] Automation write slice (ADR-0027 §3; read playback of volume and pan
      landed on `t4-automation`, session format 13, protocol 32):
  - [x] Touch, Latch and Write (protocol 33: `ParameterChange` gesture
        and pan; one "Automation Write" step per pass, Write to Latch after
        it; Trim split out below). Per-track live override atomics
        (value, touched) set by the control thread from `ParameterChange`
        with touch and release, played instead of the lane, ramped; a fixed
        `RingBuffer` of `{track, param, frame, value}` written by the audio
        thread for each block a written parameter is live (full: counted,
        never waited on); the control thread drains it into a buffer
        reserved at pass start and applies one thinned `EditAutomation` per
        lane in a `Compound` "Automation Write" at the pass's end. Touch
        returns at the glide time; Write falls back to Latch after a pass.
        `SetMix` then accepts the four modes.
  - [ ] Trim: a relative dB lane summed with Volume in dB; `SetMix` still
        refuses it.
  - [x] The interface: an automation mode selector per track, and the fader
        and pan knob sending `Gesture::Touch` at press and `Release` at
        release (#187; found done by #245).
  - [x] (#282) Touch's return to the static value (also with no lane) and the engine preference
        `--automatch-ms` (not a session setting, ADR-0027 §8; default 250 ms). ADR-0027 §8 slice 4.
        - [ ] (#282) UI: the fader cap snaps back to the static at a written release while the audio
          glides; follow the engine's played gain (post-fader gain in the state block, or a local
          glide over the AutoMatch time).
        - [ ] (#282) UI: `panDrag` saves pan to the document per step even in a written ride; pan
          keeps its static the way gain does.
        - [ ] (#282) Pro Tools-style Latch/Write that keep the static, as Touch now does: `apply_mix`
          leaves live or latched parameters alone while rolling and the engine restores the static at
          the pass's end (B1 alternative not taken; today `MixModel::writes()` is Touch only).
        - [ ] (#282) Engine: a new plan in the middle of a return (any edit) cancels the return and the
          audio snaps to the target over one callback.
        - [ ] (#282 review) `test_touch_with_no_lane_returns_to_the_static_value` compares the audio to
          the test's `expected_return()` model within 1e-4, not to the lane `automation_pass_edits()`
          writes; add an end-to-end check of the played gain against the written lane at the same frames.
        - [ ] (#282 review) A Touch ride begun while rolling and released after a stop is neither saved
          by `MixModel` (`ride_wrote_` sticks) nor restored by `touch()` (Stopped): restore or save on that path.
          Related edge: a Touch moved while STOPPED and released after the transport started saves no
          static (`endRide`: `ride_wrote_` should count only moves made while rolling).
        - [ ] (#282 review) A released Touch on a track with no region in the plan (an empty,
          input-monitored track) gets no glide (the return loop covers only `plan.tracks`).
        - [ ] (#282 review) `PlaybackController::touch()` clears `touched` before it stores the static,
          so the audio thread can aim one callback at the ride; store the static first, or make
          `return_to_` latch the static at the release (one-callback audio/writer difference).
  - [ ] AutoMatch time in the UI Preferences (a protocol message).
  - [ ] Latch/Trim and Write/Trim (mode pair, session format bump), slice 5 of
        ADR-0027 §8. Researcher first checks Pro Tools' Trim range. The engine half (Trim lane,
        session format 16, window 13, protocol 39) is #290 (open at this chore).
    - [ ] Slice 5 UI half, after #290: Trim in `EditController::set_automation_mode`; Write and Trim in
      `AutomationModeButton`; `VolumeTrim` touch and release from the fader; the trim offset shown; a
      Volume Trim lane in the lane menu and in `automation_edit`; screenshots of every scene touched.
  - [ ] Automation write slice, ADR-0027 §8 order: (1) writer hardening (done, #252), (2) plan-move and
        Load state (done, #262), (3) per-frame sends and PDC-aligned lanes (done, #271, #279),
        (4) Touch's return and AutoMatch (done, #282), (5) Trim (engine half #290, UI half open); slices 4 and 5 touch `session.cpp`,
        so one after another. Slice 5 must take session format 16, window 13, protocol 39: #277
        (names and colours) took 15, 12 and 38.
  - [x] (#262) `finish_pass` returns before Write->Latch when every lane of the pass is dropped
        (#252 review).
  - [x] (#267, #262 review) `follow_track_owners()` has a fast path when the owners are unchanged;
        `AutomationWrite` has the `is_trivially_copyable` and `sizeof <= 48` static_asserts; each write
        carries its plan row's document track id (`track_id`), so a reorder never files a ride in another
        track's lane.
        - [ ] Bound `follow_track_owners()` for Load and full reorders (still n*n) if engine track counts
          go past ~128 (#262 review N3).
        - [ ] The touch or release in the ~2 blocks between publish and `adopt_track_mix()` reaches the
          audio thread late; make it live if adopt can move the atomics without the race.
  - [x] (#267, #262 review N2) `adopt_track_mix` ran against the residency gate; fixed in
        `PlaybackController::show()` (a plan built in the adopt window now carries its own mix serial),
        with tests that fail on main.
  - [x] (#271, #279) Sends follow automated volume and pan per frame (ADR-0027 §2), and share the
        fader's per-grid lane evaluation (memo per mix call; ctest `sends`
        `test_shared_lanes_are_bit_identical`, `test_worst_block_cost` at 128 tracks x 10 sends).
  - [x] (#271) A track behind an insert chain reads its lanes `L` frames late, aligned across the loop
        wrap (`lane_frame`); only the rows whose owner moved snap their mix (`snap_row_`), the others ramp.
  - [x] (#271) The `AutomationWrite` ring comment says 12 MiB at 128 tracks after the power-of-two
        round-up; `follow_automation` looks ids up in a per-drain sorted id list.
  - [x] (#295) JACK backend: `callback_ns` never decreases (`drivers::MonotonicStamp` clamps to the previous stamp).
    - [ ] (#295 review N1) Count `MonotonicStamp` clamps and the largest one (relaxed atomic, audio-thread
          writer) and have `callback_ns_is_the_monotonic_clock` print them, so a jackd2 DLL step backwards or a
          provisional offset replaced shows up instead of being absorbed.
    - [ ] (#295 review N2) `test_clock_offset`: a case where `cycle_ns` steps back with the offset unchanged
          (jackd2 DLL resync), and correct the test comment: the offset step is not the only cause and the
          rig's period is 2.67 ms, not <= 1 ms. Unverified: the jack conformance run and the RTSan repeat.
  - [ ] (#271 review) `Session`: mark `auto_wrapped_` from the playhead (not `auto_start_`) so a wrap
        during an invalid-plan callback still counts for lagged lanes (at most `L` frames wrong).
  - [ ] (#267 audit) `orphan_writes_` does not count writes with `track_id == 0` (a roll on the initial
        empty plan); count them or say why not.
  - [ ] (#279) A forward cursor for `auto_lane_value()` in the fader's lane path (same result as the
        binary search, reset on seek and wrap, own identity test), and `apply_track_fader()`'s frame
        loop as stretches, as `sum_send()` now does.
  - [ ] (#279 review) `engine.core`: `test_shared_lanes_are_bit_identical` also covers a Touch release
        mid-roll (the `return_at_` return ramp) and a plan republished mid-roll with different lanes,
        both while the lane memo is warm.
  - [ ] (#279 review) `engine.core`: `test_worst_block_cost` (3 x 128-track rigs, 65,536-point lanes,
        320 blocks each) is timed under the sanitizer jobs; cut its block count when sanitized if it
        adds more than a few seconds there.
  - [x] A Load while rolling must ignore the rest of the open pass: `drop_pass()` resets neither `pass_open_` nor `auto_latched_`, and Load does not stop playback, so writes from the old plan can land on the new session's tracks by engine index. `drop_pass` records the pass to skip and the engine clears `auto_latched_`; test it (on PR #178, #178 review). Done: `pass_dropped_` from `Session::automation_pass()`, `Session::forget_latches()` (`t1-load-drops-pass`).
  - [x] (#262) A roll that begins between `drop_pass()` and the Load's `show_document()` opens a pass on the old plan that is not dropped (play just before a Load, or a count-off ending): its Write tracks' writes land on the new session's tracks by index. Drop by the plan the pass started on, not by the pass number read at `drop_pass()` (`engine_control_server.cpp` ~1372 vs ~2647; narrow window, needs two commands back to back).
  - [x] (#225) Apply a ParameterChange's Release (clear touched) even while tracks are moving: the flag is only set or cleared when `!tracks_moving()`, so a Release during a track move is lost and the track keeps playing its fader and writing until the client disconnects or a Load (on PR #178, #178 review).
  - [x] (#262) Carry each track's touched bits in `PlanTrackMix` so the ~2 blocks between a moving plan's publish and `adopt_track_mix()` do not read the previous occupant's hold (stray `stamp_writes` on the arriving track's lane) (#225 review).
  - [x] (#262) Move `Session::auto_latched_` with its track when a plan moving tracks is adopted: Latch while rolling through a reorder stays on the engine slot, ADR-0027 §3 (#225 review).
  - [ ] Touch on a track with no lane: make a release's audio and its written return agree. Return the live fader to the static value, or write no return; today the writer writes a 250 ms return to the static fader but the audio keeps the ridden value (on PR #178, #178 review).
  - [x] (#252) A pass whose lane overflows `kMaxAutomationPoints` must keep the other lanes: thin harder or drop only that edit. Done: thinned again at twice the tolerance up to 4 times, then the lane alone is dropped and named in a held notice (on PR #178, #178 review).
  - [x] (#252) Tests for automation: a Load or a client leaving clears the touched flags; long-pass growth (on PR #178, #178 review).
  - [x] (#252) A merged line's end value `p.at(end)` can differ from the next span's start by one float ulp, which `thin_automation` keeps as a same-frame step pair: snap a line's end to `to` when the line is not cut (suspected; #178 re-review).
  - [ ] `PassWrites::add` finds the lane by a linear scan per write: index by (track, param) if tracks pass ~64 (#178 re-review).
  - [x] (#252) `follow_automation` clamps NaN pan, since `std::clamp` passes NaN through (#178 re-review).
- [x] (#232) Pro Tools record-ready: Record arms the transport, Play starts the take, Stop
      cancels; engine state published to every client (state block layout 6,
      protocol 37, ADR-0009 update of 2026-11-01). Keypad 3 is `transport.record_ready`;
      Ctrl+Space and F12 stay `transport.record`.
- [ ] Record-ready follow-ups from #232: (N1) the keymap essentials should count
      `record_ready` as a way to start a take only when the profile also binds a working
      Play key (the engine never installs it without Play, so no shipped build is
      affected). (N2) In Qt a playhead move from a marker, the counter or a timeline click
      reaches the engine as `transport.locate`, which clears record-ready; check go to end,
      rewind, fast forward and nudge, and add conformance cases for them. (Review) The
      numpad_3 research row and the lines ~655/705 of the key-bindings note still carry the
      old "ambiguous / keeps transport.record" wording ahead of the update sentence.
      rt-engineer + ui-engineer.

### Track 2: model (rt-engineer, ui-engineer review)

1. [ ] `common/json` and `common/model`, in flight on
       `feature/timeline-model`: snapshots, operations, undo, region
       operations.
2. [ ] ADR-0009 proxies before M8 merges: Unverified 3, 4, 6 as written; 5
       and 7 on the laptop, labelled proxies (ADR-0009 update 2026-10-13).
3. [ ] Session reader and writer; fades and crossfades; track types;
       templates, autosave and backups.
       - [x] Save As, Save Copy In, autosave and rolling backups
             (`daw.model.session_mgmt`, `t2-session-mgmt`, protocol 29,
             ADR-0018 update of 2026-10-27): ModelAction::SaveAs links or
             copies the media into a new folder and moves the session
             there; Save Copy In stays, with or without the audio; the
             engine autosaves every 60 s and after each take
             (`--autosave-seconds`); each save keeps the replaced file, 20
             by default (`--backups`), pruning only this stem's rotated
             backups and never a symlink. File menu, Save As dialog,
             Shift+Ctrl/Cmd+S (unverified).
       - [x] Session templates, model half (`daw.model.session_mgmt`, ADR-0030, #272): the
             on-disk layout, `SessionModel::saveAsTemplate`/`templateList`/`newFromTemplate`
             (regions and sources dropped), no format or protocol bump. Record and the other
             take-starting commands are refused while the engine sits in a template's folder
             (`take_commands()` in `contracts/api/command.h`).
       - [x] (#291) Template menu and browser: File > Save as Template... with a name and a
             "Markers" check box calling `saveAsTemplate`; New from Template, a list beside Open
             with date, track count, rename and delete, `app.save_as_template` /
             `app.new_from_template` (no Pro Tools default key exists, none bound); screenshots.
             - [ ] (#291) Rename a template through the engine once a template action exists, so the name
               inside the file follows the folder's (today the folder and file are renamed, the stored name is not).
             - [ ] (#291 review) `templateIsFree` compares absolute, not canonical, paths; use
               `canonicalFilePath` for the engine folder and the template folder.
             - [ ] (#291 review) Click Rename and Delete in a live window and record it in a PR (only
               model-level tests exist).
             - [ ] (#291 review) Template Delete: clear the read-only attribute before removing a file on
               Windows and stop at the first failure, so a 0444 plugin file (#289) cannot leave a
               half-deleted template; `remove_without_following` re-checks `is_link` on each directory
               before descending (or `openat`/`O_NOFOLLOW`) to close the enumerate-to-recurse swap.
             - [ ] (#291 review) `test_session_templates`: nested-symlink, dangling-link, `.sonora-link`
               and `.WAV` cases in `delete_never_leaves_the_templates_folder`.
       - [ ] `NewFromTemplate` engine action (protocol bump): the engine makes the new folder from the
             template and enters it, never the template's folder (ADR-0030 §3). Until then New from
             Template writes into the template folder on every use (`ActiveFolder::enter` makes
             `Audio Files/`, an engine autosave goes to its `Session File Backups/`) (#272 review).
       - [ ] Template categories (one subfolder each, Pro Tools' template groups); grey out a template
             newer than this build (`file_format_version`) in the browser.
       - [ ] (#272 review) `SessionModel::inTemplateFolder` compares `cleanPath`, not canonical paths; a
             symlinked templates dir (or `$SONORA_TEMPLATES_DIR` == sessions dir) misroutes Save As
             after a failed New from Template.
       - [ ] (#272 review) Template names accept Windows-reserved names (CON, NUL), trailing dots and
             spaces before the extension; refuse them in `session_name_problem` before the ASIO port (ADR-0008).
       - [ ] (#272 re-review) `take_commands()`: `transport.toggle_cycle` (and `cycle_selection`) turn on
             loop record like `toggle_loop_record` but are not in the list, and the catalog test filters
             by id substring so it misses them; add them or match on what a command does, not its name.
       - [ ] (#272 re-review) The engine's `starts_a_take()` and the conformance list name the take-starting
             commands separately from `take_commands()`; derive them from it.
       - [ ] (#272 re-review, suspected) `making_from_template_` may stay set if the engine dies with the
             Save As in flight; make sure the disconnect path calls `openWasLost`.
       - [ ] Save Copy In's session-format option: write an older format
             version for an older build; needs a downgrading writer in
             common/model.
       - [ ] Autosave of an untitled session: today only a session with a
             file is autosaved, so a crash before the first Save keeps the
             takes (orphan offer) but loses the edits; an autosave in the
             untitled folder must not look like data to the empty-folder
             cleanup or block the first Save's rename.
       - [ ] Preferences for the autosave interval and the backup count
             (engine settings over the wire; command-line only today).
       - [ ] Recover an autosave in one step: opening it today saves in
             place in "Session File Backups/"; offer "restore over the
             session file" instead (Pro Tools' recovery prompt).
       - [ ] Save As progress for large copies across volumes: one Notice
             at the start today; per-file progress and a cancel.
       - [x] a failed autosave leaves covered_revision_ at the revision it
             tried, so it is not retried until the next edit; restore it on
             failure (#164 review; `t2-session-followups`)
       - [x] sweep_partial_copies deletes every "*.part" in Audio Files/ and
             MIDI Files/ on every folder enter and misses subfolders; match
             only "<media name>.part" and recurse like save_as (#164 review;
             `t2-session-followups`)
       - [x] a Save As copy is not paced while a take may start; refuse
             transport.record during a Save As copy, or pace the copy per
             checkpoint (exFAT, no hard links) (#164 review; refused, with
             toggle_record and punch, in the engine and FakeEngine;
             `t2-session-followups`)
       - [x] a Save As that fails in a pre-existing empty folder leaves its
             links/copies there, so a retry is refused "not empty"; remove
             what this call put in it (#164 review; `t2-session-followups`)
       - [x] finish_save_as fences the saver before it knows the folder can
             be entered; check folder_change_refusal() first (#164 review;
             `t2-session-followups`)
       - [x] a test of autosave_file_of for rotated backup names, C++
             rotated_backup_stem against the Qt regex, so the two cannot
             drift (#164 review; `t2-session-followups`)
       - [x] SessionFolder::enter cleared autosaves_written_ on a folder
             switch, so a returning autosave moved the saver's own autosave
             aside as a crash's; keyed by path, never cleared (#164 review;
             `t2-session-followups`)
       - [x] #157/#158 review follow-ups (`t2-timeline-followups`):
             region_fades test_fade_gains_by_hand pins each shape's gain at
             hand-computed points, independent of `fade_in_gain`;
             playback_control test_duplicated_region_is_heard_twice (a
             second PlaceRegion of one source, both copies through
             process()); a centred crossfade moved to one side at full
             length says "Crossfade moved before/after the cut", not
             "shortened" (ADR-0021 §4; ui_edit test_fades).
4. [ ] MIDI spec and ADR, then MIDI in the model (DAW 6).
       - [x] ADR-0014 and `brain/specs/midi.md` written (`docs/midi-adr`);
             revised with the rt-engineer and plugin-engineer reviews (PR #93);
             **accepted by the owner 2026-10-19**, O1–O8 as recommended (M-0).
       - [x] M-1 timebase (`m1-timebase`): `MusicalMap` in `common/model`,
             frames ↔ ticks ↔ bars|beats at 960,000 per quarter in portable
             128-bit integers, 64-bit fast path, batch API, negative frames
             by floor division, out-of-range refused; tempo bounds (O7) in
             `settings_problem`; session format 4, v3 → v4 clamps bpm and
             rounds meter denominators with a report per change; `ui/qt`
             bars|beats, grid, snap and ruler on `MusicalMap`;
             `measure_musical_map` (GCC; no MSVC toolchain here or in CI).
       - [x] Follow-ups from the #98 review: a load repair is a
             `model::Repair` (key path, old, new, why), shown one held
             notice each in the footer until dismissed, the session opening
             modified so Save writes the current format; the Transport
             window's tempo list reads one map (n log n, was n²); the
             Bars|Beats ruler thins labels by powers of two of bars instead
             of losing them past 4096 bar lines.
       - [x] M-1 model (`m1b-midi-model`): track kinds (audio, MIDI,
             instrument); MIDI playlists of regions that are windows onto
             shared, copy-on-write sequences in ticks, events inline;
             `midi_sources`; `MidiRouting` by port name; the frames/ticks
             timebase tag on `MoveRegion`, `TrimRegion`, `SplitRegion`,
             refused by name on a mismatch (A26); `AddMidiSource`,
             `PlaceMidiRegion`, `EditMidi`, `SetMidiRouting`; session format
             5 (v4 → v5 fills `kind` and `midi_sources`; frozen
             `v5-midi.sonora`), undo window format 5, `kProtocolVersion` 7;
             FakeEngine and the engine leave MIDI tracks silent and say so.
             `SetInstrument`/`RemoveInstrument` and the `instrument` key
             move to M-5. Unverified 6 measured: over the caps (`midi.md`
             §4.6).
       - [x] Unverified 6's decision, before M-4: ADR-0015 (PR #102),
             accepted by the owner 2026-10-20 with O1–O5 as recommended:
             canonical chunks, binary sidecar files, a chunk table in the
             window, a 2^24-entry budget.
       - [x] M-1c PR 1, storage and format (`m1c-midi-storage`):
             `sha256_hex` in `common/base`; the `.msq` codec, canonical,
             bounded, fuzzed (`midi.md` §4.4.1); session format 6, each
             sequence in `MIDI Sequences/<sha256>.msq`, v5 → v6 moving
             #100's inline lists at the first save (O5); a missing or
             damaged file opens its regions silent, not editable, kept
             (O2), its reference in undo window format 6; the interface's
             saver writes sidecars tmp + `fdatasync` + rename before the
             session file with one folder `fsync`, a hash-keyed written
             set and in-memory reference sets, and cleans up per O4,
             never while recording; `ModelAction::Load` carries a session
             bundle, `kProtocolVersion` 8; `midi_max_entries` (engine
             setting, 2^24): undo evicted first, edits over it refused,
             sessions over it refused at open, a Notice from 90 %.
             Measured: ADR-0015 Unverified 2 and 6 (`midi.md` §4.6).
       - [x] M-1c PR 2, chunks and protocol: `MidiSequence` as a spine of
             canonical 4-quarter chunks with cumulative counts, shared by
             undo steps (§1, §2); the window's chunk table and a Snapshot
             in parts of at most 16 MiB, encoded off the document thread
             (§4); the budget counted per chunk; ADR-0015 Unverified 1, 3
             and 7 (rt-engineer, ui-engineer).
       - [x] M-1c PR 2 also carries Load in parts (client to engine), so
             File > Open reaches the 2^24 budget; until then a bundle over
             64 MiB is refused by name (ADR-0015 update of 2026-10-20).
       - [ ] M-1c PR 2 follow-ups: at the 2^24 target the replica's window
             decode spends about 1 s in `History::restore` recounting each
             step's charge, on the UI thread (decode on a worker, or carry
             the charges); ADR-0015 Unverified 3's attach time over the M7
             socket is unmeasured; a single chunk, or a single sequence
             file in a Load, over 16 MiB is refused by name, not split.
       - [x] Sequence-file fsyncs run on the UI thread until the saver thread exists (ui-engineer, #109): gone with M-1c PR 3; the engine's saver thread writes them.
       - [ ] A MIDI region whose sequence file is missing is not drawn differently: the first MIDI region drawing marks it ("MIDI missing") and refuses edits on it by name (ui-engineer, #109).
       - [x] M-1c PR 3 (`m1c3-engine-saver`), **before the first M-4 PR
             that records MIDI** (architect, #109): the engine is the sole
             owner of `MIDI Sequences/`. File > Save is `ModelAction::Save`
             (protocol 11, path and bytes); the engine's saver thread
             (`engine/control` `session_saver.h`) writes sequence files,
             the session file and cleans up through `common/session_files`
             (new, layer 2; the interface's writer and cleanup are gone);
             while a take is open it waits for the disk thread's checkpoint
             counter to move, one save per bump, says "Saving after the
             next checkpoint", and cleans up after the transport stops.
       - [ ] M-1c PR 3 leftovers, with M-4: takes over the budget kept
             unplaced with a Notice; rebuilding a missing sequence from its
             take file (O2, with M-4's SMF reading); ADR-0015 Unverified 5
             (capture overruns with paced sidecar writes during loop
             recording on 8 audio and 4 MIDI tracks) needs M-4's MIDI
             takes and the laptop; the session file's rotated backups and
             autosave are still item 3's.
       - [ ] Save over the 64 MiB message cap (#127 review, suspected, not
             traced): a Save is one `ModelRequest.op`, capped at
             `kMaxModelBytes` (64 MiB), while session files may reach
             256 MiB. Check what `ReplicaDocument::send` does when
             `split_frames` refuses an oversize message; if it drops it
             silently, Save waits out the patience and retries forever.
             Refuse it by name up front instead.
       - [ ] ADR-0014 §9's phases in order: M-2 backend MIDI ports, M-3 playback to ports,
             M-4 recording and SMF take files, M-5 instruments
             (plugin layout 2, exit part 1), M-6 SMF interchange (exit
             part 2), M-7 the UI.
       - [ ] Researcher, before M-2: ADR-0014 Unverified 1–3 (PipeWire
             MIDI timing, port names across replug, UMP devices) and 5
             (vendor behaviour, with the PT-RG pinning).
5. [x] Solo safe in `TrackMix` (`track2-solo-safe`): `TrackMix::solo_safe`,
       session format v3 (v2→v3 fills `false`; frozen `v3-solo-safe.sonora`),
       op codec and undo window format 4, `kProtocolVersion` 6; the engine's
       `Track::solo_safe()` exempts a track from the implicit mute in
       `Session::process()` (its own mute still applies). The Mix window's
       Ctrl-click and implicit-mute outline followed (Track 3 item 2).
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
10. [ ] `daw.model.tracks` follow-ups (ADR-0023). The core ops landed:
       RemoveTrack, DuplicateTrack, reorder and add with the mix moving at
       the plan swap, protocol 28. Left, each its own line:
       - [ ] VCA masters as a track kind: `ui.mixer.vca`.
       - [ ] Folder tracks and track stacks: `ui.mixer.folders`.
       - [x] A master track: the Master is the monitor bus (ADR-0024); aux buses and sends are
             ADR-0022's, and a track's sends go with it (ADR-0023 §3).
       - [ ] New Tracks offers mono audio only: MIDI and instrument kinds
             once MIDI plays (ADR-0014 M-3) and instruments are hosted
             (`midi.instrument_tracks`); stereo once an engine track
             captures two inputs.
       - [ ] The Duplicate Tracks dialog's choices (alternate playlists,
             inserts, number of duplicates): the model has `all_playlists`
             and `inserts`; the UI sends the defaults.
       - [ ] Logic's New Track with Duplicate Settings (Cmd+D): a
             DuplicateTrack option that copies no regions; Cmd+D is
             unbound until then.
       - [ ] Delete, duplicate and move every selected track, as Pro Tools
             acts on the track selection; the UI acts on one.
       - [ ] Reorder by dragging a track header or a mixer strip.
       - [ ] Closed takes are placed by engine position (`closed_take.cpp`,
             `track_at(document, f.track)`): placed by
             `Track::document_id()`, a closing take would no longer pin its
             track's row (ADR-0023 §5).
       - [ ] A fader ride step that lands while tracks are moving is
             dropped (ADR-0023 §4): resend the rider's last value once the
             swap is done.
       - [ ] adopt_track_mix() writes set_input(m.input) for an armed track
             during a take, unlike apply_mix(); skip the input for armed
             tracks while a take runs so a take's file never changes input
             part way (#162 review, ADR-0023 §4)
       - [ ] While tracks are moving every fader ride step is dropped, not
             just one; resend the rider's last value at adoption (#162
             review)
- [ ] Automation in the model, after ADR-0027's first slice:
  - [x] `DuplicateTrack` carries an automation flag (Pro Tools' dialog
        checkbox); today a duplicate always copies its lanes.
  - [x] Conformance (#197 review): DuplicateTrack with automation=false over
        the real and the fake engine: the copy arrives with no lanes and
        its source's mode (ADR-0023 update 2026-10-31).
  - [x] Automation follows edits: moving, cutting, pasting or deleting
        regions moves the automation under them, as Pro Tools' "Automation
        Follows Edit" option, one `Compound` with the region edit.
  - [x] (#254) Automation Follows Edit on a lane at `kMaxAutomationPoints` (65536)
        made apply refuse the whole Move or Clear Compound. Done: the region edit
        goes ahead; flat-run points in the span go first; a lane still over is left
        behind, named in a held notice (ADR-0027 §7, update 2026-11-02).
  - [x] (#263) #254 review: a document-level test that thins a lane through `EditController`
        and undoes it in one step.
  - [x] (#263) #254 review: the doubled `//` line in `automation_edit.h` is removed.
  - [ ] #254 review: sloped collinear points are not dropped, only flat runs; a linear-time
        tolerance thinning would let more full lanes follow. A Range clear on several tracks
        puts every lane left behind in one notice; per-track wording may read better.
  - [x] Owner question, default taken: automation per track (Logic) rather
        than per edit playlist (Pro Tools); revisit if the comping workflow
        wants Pro Tools' behaviour (ADR-0027, alternatives).
  - [ ] Mute, send level and send pan as `AutomationParam` values, and
        lanes on aux returns and the Master (strip-keyed, with ADR-0024's
        strips); a session format bump.
- [x] automation (ADR-0027) follow-ups, after #174 merges:
  - [x] `model.cpp` `automation_points_problem` refuses a point at `kMaxInteger`, or lets `EditAutomation`'s range reach it, so the whole-lane `EditAutomation` over `[0, kMaxInteger)` can always remove every point; today a lane loaded with a point at 2^53-1 can never be emptied (#174 review).
  - [x] `plan_builder.cpp` `gain_of_db()` calls core's `auto_gain_of_db()` (exposed in `playback_plan.h`) instead of duplicating it; `session.cpp` hard-codes -144 and 24, so a lane and a fader at one dB cannot drift apart (#174 review).
  - [ ] `brain/specs/session-format.md` automation: the 2^53 - 2 point limit tightens v13 without a version bump, because no writer (`EditAutomation`, the UI clamp) could ever store a point at 2^53 - 1, as the crossfade note does for v2 (#190 review).

### Track 3: UI (ui-engineer)

Against `common/model` and `FakeEngine` now; wired to M4 when it lands.

- [x] Marker added while playing takes the engine-side playhead timestamp, not the last UI tick (it can be off by tens of ms).
      Done: marker.add reads the state block when it runs (`ShellModel::readTransport`); a stale read or a dead engine falls back to the last tick; no protocol change.
- [x] ui.mixer.views: label the insert bank toggle "Ins F-J" so it is not confused with the sends' "F-J" in the Mix header.
- [x] Memory Locations length column uses the main counter format, not always min:sec. (format_length() in time_format; columns narrowed to fit timecode; scene edit-markers-timecode.)
- [x] (#215) `ui.editing.markers` format_length Bars|Beats: borrow the beat count from the meter at the span end, not the start (the `beats < 0` branch uses tempo_at(position)); add tests for a tempo or meter change inside the span and for a marker past the last tempo point (PR #179 review).
- [x] (#215) Regenerate docs/images/sonora-ui-edit-markers.png with the narrowed Memory Locations columns; it still shows the old layout (PR #179 review).
- [x] (#215) Fold the edit-markers and edit-markers-timecode capture scenes into one scene with a counter-format parameter (PR #179 review).
- [ ] Screenshot scenes edit-markers, edit-nudge and edit-repeat fail with "session is still opening" because they send commands before the engine answers Load: give ShotContext a shared wait-for-open helper instead of per-scene retries (PR #179 review).
- [x] Capture scenes edit-markers, edit-nudge, edit-repeat failed with "session is still opening": ShotContext got a shared when_open() (screenshot_scene.h), used by them and by edit-tracks and edit-markers-timecode in place of the per-scene retries; edit-markers picture regenerated.
- [x] (#255) Same "still opening" refusal remains in the edit-undo-history scene (edit.repeat) and mix-solo-safe (solo safe); wrap their apply in when_open().
- [ ] Capture rig `when_open()` (`screenshot_scene.h`) follow-ups (PR #184 review):
  - [x] (#255) Print a stderr line when `when_open()` gives up after 30 tries, so a missing open is not silent.
  - [x] (#255) `when_open()` probes with Undo, which is unsafe with a non-empty history: probe with a read-only is-opening query, or document the empty-history precondition.
  - [ ] A helper that wraps `apply` to remove the awkward nested-lambda form in the scenes.
  - [ ] #255 review: the mix-automation-write scene shows three distinct modes, not four: make the second strip Trim/Off.
  - [x] (#268) #255 review: `docs/images/sonora-ui-mix-automation-selector.png` was missing on main; regenerated and committed.
- [ ] `ui.mixer.sends` follow-ups (ADR-0022):
  - [x] Send and aux fader rides as live parameters (`ParamId` for send level and aux gain): today each step is a SetSendMix or SetBusMix, a revision per step.
        Done (engine side, protocol 34, ADR-0022 update of 2026-10-31): `ParamId::SendLevel` and `AuxGain`, addressed by `ParameterChange::id`.
  - [x] The Mix window's send and aux faders ride with `ParamId::SendLevel` and `AuxGain` (touch, move, release), one SetSendMix or SetBusMix at release; send_model.h still sends one per step.
        Done: press, moves and release around levelDrag and auxFaderDrag; steps are live, the release is one SetSendMix or SetBusMix. Touch/Release gestures are not sent (the track fader sends none either).
  - [ ] Aux return meters, post-fader, in the state block (state layout bump); the aux strip shows none yet.
  - [ ] A send removed by a plan swap stops on the swap's block; fade it out over one block, as a chain's miss is faded.
  - [ ] Pro Tools' expanded send view (a send's fader on the strip) and Alt-click "assign to all selected tracks"; a key binding for the A-E / F-J bank, `"verified": false` until checked.
  - [ ] Two sends of one strip to one bus, which Pro Tools allows (ADR-0022 open question): key the delay-line pair on the send.
  - [ ] Releasing Alt mid-drag falls back to the old per-step SetSendMix path in `levelDrag` and `auxFaderDrag`: keep the ride on the live parameter until release (on PR #194).
  - [ ] The refused-save revert for send and aux rides is untested: it needs a refusal hook on the fake engine or document. The "send being ridden is gone" path can be driven by removing the send through the controller mid-ride (on PR #194).
  - [ ] `test_sends` `ride_ends_are_never_lost`, aux-removed scenario, asserts only `revision() >= rev+1`, so it passes without the save: assert `revision == rev+2` and that no revert is sent for the gone bus (on PR #194).
  - [ ] A send moved to another slot mid-ride is treated as gone, so the engine is not reverted: track the send by identity, not slot (on PR #194).
  - [ ] `sendLive` does not skip a step whose clamped level is unchanged: skip it, as `panDrag` will (on PR #194).
  - [x] Send reassigned in the same slot (Assign Send) kept send_gain_ across the swap, so the new bus started at full level: the session stores each send's destination AuxSum and ramps it in from silence when it changes (#163 review). The old bus's fade-out stays with the removed-send line above.
  - [x] A track's first send (or its last one's removal) changed its bus edge's pair (TrackOut to TrackFader) and reset its PDC delay line, a dropout of the compensation delay on the dry path: a TrackFader split for sends alone keeps the TrackOut's id (#163 review).
  - [x] engine/core test_sends: a send kept across a plan swap (other edits republish) continues its ramp and delay line with no step in the output (#163 review).
  - [x] FakeEngine refuses AddBus past a configurable max_buses (default 32) with the engine's wording; conformance case for it (#163 review).
  - [x] PDC line budget with sends: a return with no input takes no line, and the edges into the bus get lines before the sends. Measured: at 18 tracks, 32 buses and every send slot used, the dry mix lost lines before; 128 lines stay the default, documented in PlaybackConfig (#163 review).
  - Aux inserts, output assignment, solo and I/O-setup bus paths are `ui.mixer.io`.
- [ ] `ui.mixer.io` follow-ups (ADR-0024). Landed: every strip's output (the Master or an aux bus, subgroups, loop-free, PDC-aligned), aux solo with implicit upstream solo, the Master strip (the monitor bus), the output chooser (protocol 30, session format 12). Left, each its own line:
  - [ ] compensate() treats a bus with no send as silent and releases its return's delay line on the swap that removes the last send: keep the line while a removed send fades out, and drop the silent shortcut for a bus with an active insert (aux inserts) (#165 review)
  - [ ] Aux and Master inserts: PluginHosting resolves chains by TrackEdit; key it by strip so an aux return and the Master can host a chain, with the delay line kept while a tail plays (ADR-0024 section 5).
  - [ ] Hardware output paths and the I/O setup page: named paths mapping pairs of the device's channels (the UMC1820's 10 outputs), an output naming a path, a sink per path aligned with the Master; needs the device's channel counts in Hello (ADR-0024 section 5).
  - [ ] Aux and Master meters in the state block (a layout bump), and the Master's output pair chosen in the Mix window (ADR-0024 section 5).
  - [ ] Show implicit solo on the strips, outlined as Pro Tools does (ADR-0024 section 5).
  - [ ] A rerouted strip's edge starts a fresh delay line, a cut at the swap: ramp it (ADR-0024 section 6).
  - [ ] Session::process() loads every aux solo flag once for aux_solo and again in feeds_solo(); a solo cleared between the two loads mutes every unsoloed track for one block. Load the flags once per callback (#168 review)
  - [ ] test_routing: add a PDC case for a latent insert on an intermediate bus (track -> bus A -> bus B -> Master against a dry track) (#168 review)
- [ ] `ui.mixer.views` follow-ups (D2, after #175 merges):
  - [ ] Emit `sessionOpened` also on the "Opened the unsaved recording" recovery path (`session_model.cpp` ~1545), with a test (#175 review).
  - [ ] `test_mix_layout` resets views by emitting `sessionOpened` by hand; add a check that a real `openSessionFile` emits it and a refused Open does not (#175 review).
  - [x] Label the insert bank toggle "Ins F-J": two bare "F-J" buttons sit side by side in the header (#175 review).
  - [ ] `test_mix_layout` asserts only that `sendBankFJ`'s text differs from "Ins F-J"; pin it to "Sends F-J" (PR #182 review).
  - [ ] At 1280 px the grown F-J header buttons sit over the tail of the elided `mixNote`: bound the note's right edge to `viewControls.left` and check at 1280x720 (PR #182 review).

1. [ ] Arrange view: tracks, rulers, waveforms, regions; move, trim, split,
       edit commands, undo/redo; edit modes, Smart Tool, grid and nudge;
       zoom; fades; clip gain; markers.
       - [x] Fades and crossfades (`feature/ui-fades`): keys, Smart Tool
             corners, Clear; still open: a Fades dialog, batch fades.
       - [x] A crossfade made in the Edit window drops the fades at its
             cut, as Pro Tools does: SetFade 0 for each, then the
             SetCrossfade, as one "Crossfade" `Compound`, one undo step
             (`t3-crossfade-drops-fades`; ui_edit
             test_fades_against_crossfades). Hidden fades in documents made
             elsewhere or by older builds are still counted and named.
       - [x] common/model refuses a crossfade that overlaps the region's
             own fade at its other end, in `playlist_problem`; edge-moving
             edits, crossfade clears and punches shorten the fade instead,
             and the loader repairs older v2 files the same way, reporting
             each shortened fade on Open (PR #69).
       - [x] The take's report names any neighbour's fade the punch
             shortened (`RecordTake` runs `fit_fades_to_crossfades`). Pro
             Tools truncates the fade and never refuses the record; the
             operator still hears what changed (PR #69 review, note 3):
             `TakeAdded::fade_notices`, one engine Notice each, "take 4
             shortened the fade-in of "Gtr" on track 2 from 250.0 ms to
             240.0 ms to make room for its crossfade"; only edges the take
             did not cut are compared (`t3-punch-fade-notices`; ctest
             closed_take test_a_punch_names_the_fades_it_shortened).
       - [x] Cut, Copy, Paste, Duplicate, and Clear and Separate on a range:
             each one `Compound` with a label ("Cut", "Paste"), within
             `model::kMaxCompoundMembers` (`feature/ui-clipboard`, PR #86;
             `edit_clipboard.h`, ui_clipboard). One track's selection, as
             the selection model has; Shuffle refused by name.
       - [x] Repeat (`edit.repeat`, Alt+R, Edit > Repeat…): the key asks
             for the number of repeats in a dialog (offering the last one)
             only when there is something to repeat; the count comes back as
             the command's argument. The copies go end to end after the
             selection, overwriting what they cover, as one "Repeat"
             `Compound` whose size is counted before it is built, so a typed
             2147483647 is refused by its size; Shuffle refused by name
             (`t3f-repeat`; ui_clipboard test_repeat, test_menu).
       - [x] Multi-track selections and their clipboard: a Selector drag
             across lanes, or Select All (`edit.select_all`, Ctrl+A, Edit >
             Select All), leaves a range on several tracks (`Selection::
             tracks`). Cut, Copy, Clear, Duplicate, Repeat and Separate act
             on every track of it as one `Compound`, the member limit counted
             over the whole gesture. The clipboard keeps one lane a track,
             empty ones included; Paste puts lane i on the track i below the
             selected one, as Pro Tools does, and like Pro Tools leaves out
             lanes past the last track: the paste goes ahead and the status
             line names what was left out ("Pasted 2 of the clipboard's 3
             tracks: there is no track below Keys for what was copied from
             Keys."); a paste whose regions would all fall past it is refused.
             Fades and trims to the insertion point refuse several tracks by
             name (`t3-multitrack-clipboard`; ui_clipboard
             test_several_tracks, test_menu).
       - [x] Markers and memory locations (`t3j-markers`; ui_markers):
             `marker.add` (Pro Tools' keypad Enter, Edit > New Marker…, the
             Markers ruler's +) asks for the name while stopped and drops
             "Marker N" at the playhead at once while rolling; a selected
             range makes a selection marker over it. Each add and clear is
             one undo step. The Markers ruler under the time rulers draws
             them; a click recalls one (insertion point or range, never
             snapped, the view paged to it), Alt-click clears it. The Memory
             Locations window (`window.memory_locations`, Ctrl+keypad 5)
             lists them in the main counter's format, a click recalls, × clears.
             Still open:
             - rename a marker: the model has no operation for it
               (`AddMarker`/`MoveMarker`/`RemoveMarker` only), so a
               `SetMarkerName` op is a common/model change first;
             - drag a marker along the ruler (`MoveMarker` exists);
             - Pro Tools' memory location properties (zoom, track show/hide,
               the selected tracks) need fields in `Marker` and a session
               format change;
             - recall by number (keypad `. N .`) needs chord sequences in the
               key map schema (research §4 item 2);
             - recall moves the playhead once `transport.locate` exists
               (Track 1 item 4); a marker line through the lanes;
             - Logic's marker keys, once confirmed against its guide.
       - [ ] Shuffle's ripple for the clipboard once `ShiftRegions` exists
             (ADR-0009 update 2026-10-14, item 5).
       - [x] One clipboard lane pasted into a selection on several tracks
             fills each of them, as Pro Tools does: one "Paste" `Compound`,
             the member limit counted over every track, the selection kept
             across them. A clipboard of several lanes keeps its layout on a
             wider selection; a selected region pastes on its track only
             (`t3k-paste-fill`; ui_clipboard test_several_tracks, test_limit).
       - [ ] A clipboard copied in another session pastes nothing today
             (refused by name: its sources are not in this pool). Pro Tools
             copies the audio across; that needs `AddSource` members and the
             files copied into the session folder first.
       - [x] The compound rule for every sender (PR #75 ui-engineer
             review, item 4), for the Edit window's senders (PR #86):
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
             regions a track, with its overwrite splits, fits. Still open
             for the Mix window's Alt-click (Track 3 item 2).
       - [x] The undo and redo text: "Undo Paste", "Redo Trim", from the
             history's step labels, as `SessionModel::undoText`/`redoText`
             and the Edit menu's first items (PR #86).
       - [x] ReplicaDocument sends one Resync per loss, not one per failed
             update, and keeps the last good document on screen while it
             waits, refusing edits meanwhile (PR #75 ui-engineer review;
             PR #86, ui_replica_document).
       - [x] The Undo History window (`window.undo_history`, Edit > Undo
             History; no profile key, since neither Pro Tools nor Logic
             documents one): every step undo can reach, oldest first, the
             undone ones in italics below a line. A click on a done step
             undoes it and every later one, on an undone step redoes up to
             it, as Pro Tools does; Undo All and Redo All. Each step is its
             own Undo or Redo request, as the key pressed that many times.
             A refusal in the window (offline, behind the engine) comes
             before anything is sent; one from the engine is reported for
             its step, the steps after it already sent. A
             history that has dropped its oldest steps says undo stops at
             step 1 (`t3i-undo-history`; ui_undo_history,
             ui_replica_document a_history_jump_is_one_request_a_step).
             Still open: Pro Tools' creation times, and Clear Undo Queue,
             which needs an engine request.
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
       - [x] Solo safe (`t3-solo-safe-mix`): Ctrl-click on a solo button,
             in the Mix window or the Edit window's track header, toggles
             `TrackMix::solo_safe` through `SetMix`; the button is recessed
             with a padlock; the implicit-mute outline follows the engine's
             rule (`SessionModel::implicitMutes`), so a solo-safe track is
             never outlined while it is still heard.
       - [ ] Solo modes (latch, X-OR, momentary) and stereo pan wait for
             those and stereo tracks.
3. [ ] Transport window and counters; tempo map and click display.
       - [x] `feature/ui-transport`: bars|beats through the whole tempo map
             (counters, ruler, grid lines, snap); the Transport window
             (`window.transport`) with every format from one reading, tempo,
             meter, next change and the map, pre/post-roll, loop and punch;
             click and rolls dimmed with the reason.
       - [x] Tempo and meter rulers in the Edit window (`t3-transport-window`):
             a mark and the value at each change, the value in force pinned
             at the left edge (ui_transport test_conductor_ruler).
       - [ ] Tempo and meter editing on those rulers (a SetSettings each;
             the conductor).
       - [x] Counter entry (`t3-transport-window`): a click on the bar's or
             the Transport window's counter digits, a position in the main
             counter's format, Enter moves the insertion point and locates
             through `transport.locate`; refused with the reason held beside
             the field for text that is not a position and during a take
             (ui_transport test_counter_parse, test_window).
       - [ ] A key that opens the main counter for entry (Pro Tools'
             keypad `*` is Record Toggle in Transport mode, already
             `transport.punch`; the Classic-mode binding is unchecked).
       - [x] The nudge steps by the first entry's grid value
             (`edit_controller.cpp` `nudge`): step by the value at the
             region instead, after `feature/ui-fades` lands there.
       - [x] A stale engine read no longer makes the bar counter and the
             Transport window disagree (#62 merge gate): `SessionModel`
             keeps the last state on a stale read, as `TransportModel`
             does (ui_transport test_window).
       - [x] ui.transport: the TransportBar LOOP indicator reads loop_playback only; show loop record (e.g. "LOOP REC") so a loop take is never armed invisibly (ADR-0016, update "loop record as a record mode"). `t3-punch-loop-ui`: LOOP (CYCLE in a cycle profile), LOOP REC and PUNCH chips on the bar, each sending its key's command; LOOP REC shown in every profile (ui_transport test_bar_loop_record).
       - [x] ui.record.punch_loop, UI side (`t3-punch-loop-ui`): the punch
             range from the Edit selection and cleared
             (`transport.punch_selection`, `transport.clear_punch`, in the
             catalogue, Engine domain, run by the Qt stand-in through the
             replica as mute is), refused with no selection, with loop
             record on (C3) and during a take; the bar's PUNCH chip and the
             Transport window's Set/Clear; pre/post-roll typed in seconds
             (0-60) in the Transport window, no longer dimmed as unused,
             with the line saying where they apply (ui_transport
             test_punch_from_the_ui, test_punch_refused_with_loop_record).
             No wire change. Still open:
       - [ ] ui.record.punch_loop: Pro Tools' Auto Input / Input Only
             Monitoring switch (Option+K): a session-wide monitor mode
             beside the per-track Auto/Input the engine has (TrackMix::monitor,
             the I button); needs a session setting and the engine following
             it in `PlaybackController::apply_monitoring`.
       - [ ] ui.record.punch_loop: TrackPunch, per-track punch enable (Pro
             Tools Ultimate/Studio; Shift+Ctrl+T): the engine's punch is
             per take over every armed track, so this needs a per-track
             punch flag in the document and `start_punch` honouring it.
       - [ ] ui.record.punch_loop: QuickPunch as a record mode
             (`transport.toggle_quickpunch`, Ctrl+Shift+P), so Pro Tools'
             Record while rolling punches as `transport.punch` does; and
             Pro Tools' record of a timeline selection as a punch over it.
       - [ ] ui.record.punch_loop: pre/post-roll on and off (Pro Tools'
             Ctrl+K) without losing the amounts: a `transport.rolls_on`
             session setting (session format bump); and Option-drag in the
             ruler to set them.
       - [ ] ui.record.punch_loop: the loop and punch ranges drawn on the
             ruler (Pro Tools' timeline markers), punch in the record colour.
       - [x] #156 review N1 (`t3-transport-followups`): engine handlers for
             `transport.punch_selection` (the selection attached by the Qt
             router, as for `transport.toggle_cycle`) and
             `transport.clear_punch`, in engine/control and FakeEngine;
             refused with no selection, with loop record on, and, with a
             punch change by `SetSettings` too, while a take is open or
             requested. The Qt stand-in is gone (conformance "punch range
             and rolls at the authority", ui_play_range
             test_punch_selection_goes_to_the_engine). Protocol 25.
       - [x] #156 review N2 (`t3-transport-followups`): the punch range and
             the rolls no longer go as a whole copy of the replica's settings,
             which a stale replica could use to revert a field the engine
             changed: `transport.set_pre_roll`/`set_post_roll` (frames) edit
             one field at the engine (ADR-0016, update "transport settings at
             the authority"; compare-and-set on a base revision rejected
             there). ui_transport test_punch_from_the_ui.
       - [x] #156 review N3 (`t3-transport-followups`): `parse_roll_text`
             names the tab as `'\t'`; the bar's loop and punch chips use
             `Theme.space1`.
       - [x] #159 review (`t3-transport-roll-limit`): `transport.set_pre_roll`
             and `set_post_roll` refuse a roll past `model::kMaxRollSeconds`
             (60 s) at the session's rate, in the engine and in FakeEngine,
             not only in the Qt field, which now takes the constant from
             `common/model`. Conformance "punch range and rolls at the
             authority". No protocol change.
       - [ ] The last interface `SetSettings` built from a whole replica
             copy: the rename after an engine Save (`SessionModel::finishSave`)
             copies name and id into the shown
             settings; the engine's Save handler should apply the rename
             itself (ADR-0016, update "transport settings at the authority",
             the rule for every sender).
       - [ ] ui.record.punch_loop: a Logic key for Set Autopunch Locators by
             Selection once quoted from Apple's guide.
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
       - [x] `ui.comping.playlists` (ADR-0026, protocol 31, format 11
             unchanged): New, Duplicate, choose, Rename and Delete Unused per
             track, each one undo step; a switch is a plan swap, exact at a
             block edge; no switch while a take starts or closes, nor on a
             recording track; media of inactive or removed playlists stays
             in the pool. Left, each its own line:
       - [ ] Pro Tools' New and Duplicate Playlist dialogs (name the
             playlist first) and the Delete Unused Playlists dialog (pick
             which go); the UI uses the default names and deletes them all.
       - [ ] A refused switch the engine answers (a take starting or
             closing) reaches the status line only; hold it as the local
             recording refusal is held (`trackRefusal`), for track commands
             too.
       - [ ] Playlist selector in the Mix window's strips and a
             previous/next playlist command; Logic's take folders map onto
             `ui.comping.lanes`.
       - [ ] Pro Tools' playlist keys (`cmd+backslash`,
             `cmd+alt+backslash`) are `"verified": false` until checked
             against Avid's Shortcuts Guide.
       - [ ] A declicked switch (a short crossfade at the swap) would need
             a second voice per track in the plan (ADR-0026 §3).
6. [x] The version refusal reaches the operator (PR #75 ui-engineer review,
       item 3; PR #90). `version_mismatch()`'s text
       went to stderr and exit 1, which on the appliance is a black screen
       restarted for ever:
       - [x] at startup, a full-screen refusal page with the text instead of
         exiting (`RefusalPage.qml`, `refusal_page.h`); other connect
         failures still exit, since the unit's restart is the retry that
         can fix them;
       - [x] on reconnect, `SocketEngine::link_problem()` (its
         `last_error()`) leads the engine-gone banner while the engine is
         not alive (`ShellModel::linkErrorText`), over a gone engine's last
         alert;
       - [x] a version refusal stops the retry loop (`refused_version()`);
         only an explicit `connect()` tries again (client_socket_engine
         a_version_refusal_stops_reconnecting).
       Still open: the tty interface (`sonora-tui`) prints the refusal and
       exits; it has no page.
7. [ ] Design audit 2026-10 (`brain/research/ui-design-audit-2026-10.md`;
       spec `ui-design.md`, "Update, 2026-10-31"). One PR per line, each
       under ~150k context, in this order. Every one carries before-and-after
       screenshots and is reviewed under the spec's "Review of UI PRs".
       Findings are referred to as F1 to F37.
       - [x] D1 `ui.design.strip_layout`: fixed strip widths (Narrow 64,
         Regular 96, Wide 128) with sideways scroll instead of
         `window / count`; the master pinned right; the fader zone takes
         "the rest", with the ≥ 55 % and ≥ 35 % budgets checked by a layout
         test at 1920×1080 for 18 and 48 tracks (F1, F2). Tokens in
         `theme.h`.
       - [x] D2 `ui.mixer.views` (existing item): collapsible views for I/O,
         Inserts A–E and F–J, and Sends A–E and F–J, with A–E shown by
         default, kept with the session; "+N" chip on a collapsed bank in
         use; a section that would break the budget scrolls inside itself
         (F1). The width presets from D1 go in its View menu.
         Done: the banks (MixModel `showIO`, `showInsertsAE`, `showInsertsFJ`,
         `showSends`; the sends' A-E/F-J is SendModel::bank), "+N" chip, the
         fader floor 35 % with an F-J bank expanded, reset when another
         session opens. Left: the width presets and a View menu; the views
         live in the process, not in a file (interface state is outside the
         session format, ADR-0007 5), so they do not survive a restart.
       - [ ] D3 `ui.design.names`: one Qt-free `abbreviate()` with tests (the
         spec's rules 1 to 6), used by name plates, insert and send slots,
         the Edit header and the choosers; full names in tooltips; reasons
         wrap (F7, F14). Empty slots become blank wells (F6).
       - [ ] D4 `ui.design.strip_controls`: `Knob.qml` (spec "Knobs, faders
         and sliders"); pan as a knob, two for a stereo track; R/I/S/M at
         ≥ 24 × 24 px (WCAG 2.2), 2 × 2 at Narrow; `insertSlotHeight` 18 (F3, F9).
       - [ ] D5 `ui.design.strip_kinds`: kind band (input, AUX, MASTER);
         master Wide on `raised` behind a rule, one name everywhere; aux
         remove into a context menu; aux and master meter wells drawing the
         no-data face rather than nothing; selected strip's 2 px `accent`
         name-plate edge (F5, F8, F11, F33 aux part).
       - [ ] D6 `ui.design.strip_meters`: meter widths, an 8 px over
         indicator, scale in `textCaption` left of the slot and never under
         the cap, `meterMark` raised to ≥ 3:1 (F10). The feeds for aux and
         master stay with `ui.mixer.metering` (existing, engine-side state
         layout 3) (F4).
       - [ ] D7 `ui.design.top_bar`: priority layout; the health line never
         dropped (it moves to the banner row); nothing clipped at 1280 px;
         "Open" only in File; tab and menu labels told apart; Transport
         leaves the tab row for a Window menu; status chips drawn flat
         (F15, F17, F18, F19).
       - [ ] D8 `ui.design.type_scale`: the glance set (counter ≥ 40 px,
         names and readouts 13 px) and the UI scale preference (1.0, 1.25,
         1.5, 2.0; default from the screen's physical size) (F16).
       - [ ] D9 `ui.design.capture_rig`: 1920×1080 reference, plus
         2560×1440 for Mix and Edit, plus one 1280×720 minimum-width scene
         per window; capture mode suppresses the software-rendering banner
         (one scene keeps it); a 48-track scene; regenerate every PNG and
         delete the stale ones (F34 to F37). Touches `main.cpp` and the
         entrypoint, so it is its own PR. It has no dependency on D1 to D8
         and can run in parallel with them; it is recommended early, so the
         D-slices' screenshots are taken at the reference size.
       - [ ] D10 `ui.design.plugin_chrome`: shared plugin-window header (spec
         "Plugin window chrome") for the generic editor: parameters in
         their units; Remove and Make inactive into the ▾ menu; pin and
         reuse; placement beside the strip; scenes for the generic editor
         and a native-editor placeholder (F12, F13). The native editor
         inside it is `plugins.host.editors`.
       - [x] D11 `ui.editing.names_colours` (existing item), first slice (#277):
         colour stored per track, never recomputed from the display index
         (F21); track, clip and region names and colours (session format 15, window 12, protocol 38).
         - [ ] (#277) Pro Tools renames the main playlist along with the track; we leave playlist names alone.
         - [ ] (#277) Colour several selected regions (a range across tracks) as one Compound.
         - [ ] (#277) Esc and `x` on the palette and the Name dialog (D14 covers every window).
         - [ ] (#277) MIDI regions are not drawn in the Edit window yet; their stored colour is ready.
         - [ ] (#277) Confirm the three new key bindings against Avid's and Apple's guides and drop `"verified": false`.
         - [ ] (#277 review) ADR-0027 §8 row 5 text still says session format 15, window 12; renumber to 16, 13, 39.
         - [ ] (#277 review) model: codec round-trip tests for SetTrackName/SetRegionName with an empty name,
           a 4 KB name, only spaces, and names with NUL and newline (op_codec, test_tracks).
         - [ ] (#277 review) model: cap track, region and playlist names in bytes and refuse control
           characters in apply and the session reader, with a clear error (a hand-edited file or remote op can store a newline or MBs).
         - [ ] (#277 review) `set_track_colour` truncates a selection of more than 256 tracks to
           `kMaxCompoundMembers` without saying so; split it into several steps or refuse it (`edit_names.cpp`).
         - [ ] (#277 review) `static_assert(theme::kRegionPaletteSize == model::kColours)`, and fix the
           `edit_names_colours` scene comment ("the overheads" is the Hi-Hat, track index 2).
       - [ ] D12 `ui.design.edit_density`: header meter ≥ 6 px; track-height
         presets for 16 to 48 tracks, unless `ui.editing.zoom` already
         carries them (F22, F23).
       - [ ] D13 `ui.design.colour_meaning`: separate solo, marker, caution
         and amber; separate input from meter green; punch in `record`;
         latency OK in `text`; NO DATA moved into the meter wells (F24 to
         F29). Each new value is checked against the contrast targets.
       - [ ] D14 `ui.design.dialogs`: `×` and Esc on every window, a scrim
         behind modals, the primary-button rule, left-aligned menus with
         key equivalents from the active map, destructive actions labelled
         (F30 to F33).
- [x] `automation.lanes` UI, first slice (ADR-0027; the model and read playback
      landed on `t4-automation`): a disclosure triangle on each audio track
      header opens a Volume and a Pan lane under the track in the Edit window
      (lanes per track, Logic's; decided), the curve drawn from
      `TrackEdit::automation` as the engine plays it, a lane with no
      breakpoint drawn dashed at the static fader or pan, and a lane shown
      dimmed when the track's mode is Off. Breakpoints with the Smart Tool and
      the Grabber (click adds, drag moves and stops against its neighbours,
      Clear deletes the picked one), the Pencil (`tool.pencil`, F10) draws
      freely and is thinned at the release, the Selector selects a range of a
      lane and Cut, Copy, Paste and Clear act on it; each gesture is one
      `EditAutomation`, so one undo step. Screenshot `edit-automation`.
- [ ] `automation.lanes` follow-ups (lane height following the vertical zoom is done): a `Compound` of several lanes when a range spans tracks (the Selector is
      one lane on one track for now), Trim mode's relative edit of a range,
      the lane selector for plugin parameters (ADR-0027 §5), a lane height
      that follows the vertical zoom, and key bindings `"verified": false`
      until checked (none were added: `tool.pencil` was already F10;
      `automation.thin` is `ctrl+alt+t`, `origin: sonora`, in both maps).
- [ ] `automation.lanes` UI follow-ups from the #181 review:
  - [ ] ui test (#199 review): `automation.thin` asserts the exact kept points (a collinear 11-point ramp keeps 2 edge points), a step inside the range survives, and a pan lane uses its own tolerance.
  - [ ] keymaps (#199 review): reconsider `ctrl+alt+t` for `automation.thin` if the UI is ever run under a desktop environment (GNOME grabs it for the terminal).
  - [ ] ui (#199 review): decide whether `automation.thin` uses the engine writer's tolerance (0.1 dB) instead of Pencil's 0.25 dB.
  - [ ] review process (#199 review): reviewers of PRs that add a command or key must also run or read the `contracts/` api tests; the `api_wire` `domains()` list is hand-written, so add the id there (#199 failed CI on it).
  - [ ] `EditController::undo` and `redo` leave `sel_.point` stale: an undo or redo resets the selected breakpoint, otherwise Clear deletes whichever point now has that index.
  - [ ] `tool.next` cycles `% 4`, so Pencil is never reached: decide whether Pencil joins the cycle, and document the choice in the keymap notes.
  - [ ] ui CMake: move the `ui_automation` block below `ui_edit`'s `set_tests_properties`.
  - [ ] `test_lane_height_follows_zoom`: replace the literal `2` with `kLaneCount` and wrap the over-long `test_*` call line in `main()` (#192 review).
  - [ ] Vertical zoom keeps `scroll_y` but not the row under the cursor or the selection: anchor `scroll_y` on the selected track when lanes are open, so the focused lane does not drift off screen (#192 review).
- [ ] `ui.mixer.automation` UI: the automation mode selector on each strip
      and track header, sent with `SetMix`; the fader and pan knob follow the
      lane while playing in Read. Done: the selector on each Mix strip (Off,
      Read, Touch, Latch, Write; Trim listed and refused until the engine has
      it), the fader and the pan sending Touch on press, Move per step and
      Release on let go, the lane header's label following the mode.
      Left: the fader and pan following the lane while playing in Read
      (needs the engine to publish the played value). The Edit header
      selector and the step-mode key binding are done (#203); the list closes
      as a menu (#268).
- [ ] `ui.mixer.automation` follow-ups from the #187 reviews:
  - [x] (#268) The `engine_adapter.h` header table names `send_track_param`.
  - [x] (#268) The automation selector list closes on click-away and on Escape (focus-loss too).
  - [x] (#268) `panDrag` skips a Move when the clamped pan equals the current one; `pan_drag()` gives both the Move value and `SessionModel::panDrag`'s formula.
  - [x] (#268) `scenes/mix_automation_selector.cpp` no longer includes `<QCoreApplication>`, `<QEventLoop>` and `<QThread>` twice.
  - [x] (#268) `dropRide` and `dropPan` agree: a Release for an index past the document's tracks carries the hold's last value.
  - [ ] (#268 review) `AutomationModeButton.onOpenChanged(false)` sets `setTextEntry(false)` even when a text field took focus in the same turn; set it only if `list.activeFocus` was the holder.
  - [ ] After a track removal, a dropped hold's Release lands on whichever engine track now has that index and can end another interface's Touch: send it by track id once the protocol carries one (suspected).
  - [ ] No test covers `Component.onDestruction` -> `finish()`: a QQmlComponent test loading `ChannelStrip.qml` with a MixModel is feasible but needs a small QML test rig.
  - [ ] (#268) `test_ui_mix` prints `SendWindow.qml:177: TypeError: Cannot call method 'endLevelRide' of null` (also on main): add a null guard.
- [ ] `session.cpp:1916`: while `from` is less than the chain latency and not
      looping, `t` clamps to 0 and then advances, so a lane changing in the
      first frames saws. Hold at frame 0 until `from + i` reaches the latency.
- [ ] `test_automation.cpp`: add a test that a track behind an insert chain
      reads its lanes `chain.latency_frames` early; the `lag` path has no test.
- [ ] (#274, #287) `test_inserts` (`ui_inserts`): the knob-drag flake (CI run 41518271936) now prints a
      "knob drag:" stderr diagnostic when the check fails; #287 waits until the knob is what a press
      reaches before dragging it (an unproven cure). If the flake recurs, look at the press path
      (QQuickWindow delivery), not geometry; if it stays away, remove the
      diagnostic or turn it into a CHECK message.
- [ ] (#287 review) `test_inserts`: `mouse_target` walks `childItems()` in insertion order, not Qt's
      z/paint order, and ignores clip, opacity and pointer handlers; use the delivery order if a failure
      names an item the test said was reachable.
- [ ] Test hygiene: audit fixed sleeps before SIGKILL/deadline checks in
      engine/app/tests/test_exit_watchdog.cpp and
      engine/control/tests/fake_plugin_host.cpp for the ctest -j + TSan load
      flake class fixed in test_take_kill (#195); replace with condition waits.
- [ ] `test_exit_watchdog` prompt-exit case: arm the watchdog with a ~20 s
      deadline, so the prompt-exit path is tested without load-sensitive
      bounds (#205 dropped those from the other cases).
- [x] (#231) The take in progress grows on each armed track's lane while the transport records
      (`ui/qt/src/live_take.{h,cpp}`, the `record` colour, from `take_peak`; screenshot
      `docs/images/sonora-ui-edit-recording.png`).
- [ ] Live take follow-ups from #231: (a) the live overview ignores the input latency the
      engine shifts the take by (`take_latency_`); use the measured latency from the Latency
      event. (b) Once real peak files exist (`ui.editing.waveform`), the closed take keeps the
      live peaks until its peak file is written. (c) The fake engine places no region when a
      take stops, so on the fake the live region disappears after 2 s: have it place one.
      (d) `documentUpdated()` clears the live region on any update after Stop, not only the
      take's region (an unrelated edit in the 2 s window drops it early), and a region that
      arrives before the Stopped tick leaves the live region drawn over it until the 2 s cap.
      (e) The first tick of a take can carry the pre-record input peak when no meter was
      reading that ring. ui-engineer.
- [x] (#234) Qt Record button arms under Pro Tools (`transport.record_ready`), blinks at 1 Hz,
      held refusal under the bar (`RecordButton.qml`; screenshot
      `docs/images/sonora-ui-transport-record-ready.png`).
- [ ] Record button follow-ups from #234: `protools.json` has no Stop chord (only Space),
      found in #231; a QML tooltip on `RecordButton` (only the accessible description now);
      wire the reduced-motion setting to `RecordButton.steady`; a Play hint while
      record-ready. ui-engineer.
- [ ] Pro Tools parity audit (#229, `brain/research/protools-parity-audit-2026-11.md`); its
      top-15 lines, minus the three that extend open items (`io_setup` is the hardware
      output paths and I/O setup page item, `auto_input` and `preroll_toggle` are
      `ui.record.punch_loop`). Each is ui-engineer unless said, effort in brackets:
  - [ ] Arm modifiers: click on a track's R button arms it; Option-click arms or disarms all
        tracks; Shift-click arms the selected tracks; the same on the I button (S).
  - [ ] An armed track meters its input while stopped so the preamp can be set; unarmed tracks
        meter playback. Confirm today's behaviour first (M).
  - [ ] File > New Session dialog: name, location, sample rate (44.1, 48, 88.2, 96 kHz), bit
        depth (16, 24, 32 float); say how the engine handles a rate change, and whether bit
        depth is a session field (M).
  - [ ] New Tracks panel adds mono/stereo, Audio/Aux/Master, and a name (S).
  - [ ] Tab and Ctrl+Tab move the edit cursor to the next/previous clip boundary and, with Tab
        to Transients on, the next transient; detection needs a spec first (M).
  - [ ] Confirm `edit.heal_separation` and `edit.consolidate` act in the Qt UI; wire them or
        take them off the key map (S).
  - [ ] Zoom presets 1-5 stored and recalled, Zoom Toggle on E, R/T live where Pro Tools has
        them (S).
  - [ ] A visible Link Timeline and Edit Selection toggle (Shift+/) with its state in the
        toolbar; folds into `ui.editing.selection` (S).
  - [ ] Recall a memory location by number from the keypad; needs the chord-sequence work in
        the key-bindings note, section 4 (M).
  - [ ] Spot mode opens a dialog for a start time in the chosen format; Relative Grid as a
        second Grid mode (M).
  - [ ] Promote a playlist or lane to the main playlist, ahead of the lane view in
        `ui.comping.lanes` (M).
  - [ ] Setup grows from Latency only to Playback Engine (device, buffer), Click/Countoff and
        Preferences; folds into `ui.workflow.preferences` (M).
  - [ ] Correct the audit note: row 11 says zoom in/out are live only in Commands Focus
        although Ctrl+[ and Ctrl+] are bound in `keymaps/protools.json`; `tool.zoom` (F5) and
        `tool.scrub` (F9) have no handler in `ui/qt/src/session_model.cpp`, so row 9 "works"
        overstates (F9 should refuse loudly rather than lie); give each line an owner role.

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
       - [x] `SupervisedChain` into the control server (checklist 5),
             with the ChainResolver filling ChainBinding from it
             (`PluginHosting`, `feature/plugin-control-server`): chains
             from the document diff, the amendment of 2026-10-14's item 1
             and changes A, B, C, F, H, retiring by epoch, PluginFault from
             the chains, PluginParameterChange by Loaded key, gestures to
             SetInsertParams/SetInsertBypass. ctest
             `plugin_hosting_{open_starts_silent,new_insert_dry,edit_keeps_old,undo_abandons,missing,refused,params,down_keeps_latency,gestures,server}`.
             #80 review round: values changed while a host starts are
             posted once its store is in a plan (B2,
             `values_while_starting`); a session opened, even the same
             one, starts every chain afresh, silent until Ready (B3,
             `reload_starts_afresh`); changes tagged with the playing plan's
             epoch or the first plan with the store (B1); a gesture left
             open by a dying host goes to the document (`gesture_left_open`);
             a failed edit is faulted and not respawned per change
             (`failed_edit`); the old host alive while a plan names it
             (`edit_keeps_old`, residency held); teardown asks every host
             first and waits once (2 s for the set).
       - [x] pidfd instead of polling the host processes:
             `ChildProcess::exit_handle()` (a Linux pidfd, close-on-exec,
             -1 elsewhere) and `HostProcess::exit_handle()`; every bounded
             `wait()` (host stop, replace, scanner) sleeps on it instead of
             a 5 ms waitpid loop. ctest `platform_child_exit`. The control
             server has no poll set yet: putting `exit_handle()` there,
             beside `socket_handle()`, is M3's.
       - [ ] `ChildProcess::wait` and `exit_fd_readable`: treat poll revents
             other than POLLIN (POLLNVAL, POLLERR) as "fall back to polling",
             not as exit (#183 review).
       - [ ] `platform_child_exit`: skip (exit 77) when `exit_handle()` is -1
             (pidfd_open ENOSYS/EPERM) instead of failing (#183 review).
       - [x] Scan worker and catalog cache (ADR-0012 amendment of
             2026-10-19, items 1-7, Q1-Q3; Q4 landed with #87):
             `contracts/plugin`
             probe_record.h (record version 1, one codec, golden line;
             `--version` names `probe 1`); `--list` names each record's
             bundle, `--bundles-only` runs no plugin code, `--only-given`
             enumerates only the changed VST3 bundles; `PluginScanner`, one
             thread owned by the control server, lowered to nice 10 and the
             idle I/O class before its walk and its children, a try_lock
             exchange that never drops a request, the cache in
             `user_cache_dir()` resolved by engine/app (`--plugin-cache`;
             header with the host line and the os-release build id),
             fingerprints that follow symlinks and take max(mtime, ctime),
             crashed and hung probes cached blocked, one slot reserved for
             chain starts, no catalog child during a take, `plugin.rescan`
             refused during a take; PluginHosting's probes through the
             worker; AddInsert/ReplaceInsert resolved against the catalog
             with its name and version stored. ctest plugin_probe_record,
             platform_background, plugin_paths, plugin_scanner (15 rules
             on a fake host), plugin_catalog, plugin_host_list_bundle,
             plugin_host_list_bundles_only, plugin_host_probe_record,
             plugin_host_probe_not_installed.
       - [ ] Scan follow-ups (architect on #95): the image build writes
             BUILD_ID into /etc/os-release, before OS 1 (also under Phase 1
             below; the cache header reads it, empty until then).
             Unverified 11 (warm time to generation 1 on the
             stick, Debian's set plus a yabridge set) and 12 (researcher).
             The PR that hosts editors moves the probe record version or
             the host `--version` line (ADR-0012 item 2 note).
       - [x] Scan follow-ups from #95: a catalog whose record for a key
             changed or lost it drops PluginHosting's cached answer (a
             rescan no longer leaves a stale parameter table; a running
             chain keeps its own until its next start); the worker splits
             the encoded catalog into frames once and every client is
             handed the same immutable frames (`Outbound` takes a shared
             `Frame`); `exchange_frees_nothing` counts destructor calls on
             the control thread instead of timing them. ctest
             `plugin_hosting_rescan_drops_stale`, `plugin_catalog`
             (shared_frames), `plugin_scanner` (exchange_frees_nothing).
       - [x] FlushPluginState through GetState and the saver thread's
             durable write; today it still answers from the document.
       - [x] Play waits for plug-ins at session open (owner, 2026-10-18):
             `transport.play`/`toggle_play` from a stopped transport is
             held while `PluginHosting::loading()` is non-zero, at most
             `Options::plugin_open_wait_ms` (10 s) from the open, with a
             Notice "Loading plug-ins (n of m): play starts when they are
             ready", said once per wait and never kept for a client that
             attaches later; only a second space, a stop or a take
             withdraws it; record is never held; with hosting disabled
             nothing is loading. ctest `plugin_hosting_play_waits`
             (answered after the plug-in's load, before the bound; two
             presses, one notice), `plugin_hosting_play_wait_bounded`
             (answered at the bound, 800 ms in the test, with the plug-in
             still loading), `plugin_hosting_play_no_hosting` (a host of
             another kind: no wait), `plugin_hosting_play_wait_cancel` (a
             return to zero leaves it waiting, a stop cancels it).
             The UI showing the notice as a progress line is the UI's.
       - [ ] A Notice cannot be cleared (contracts/api has no clear for
             it): the "Loading plug-ins" line stays on the screen of a
             client attached during the wait, until the UI drops it.
       - [x] A new chain's start that fails while the old chain still
             runs is shown as a fault on the edit's inserts, and not
             started again until the document wants another chain
             (`plugin_hosting_failed_edit`).
       - [x] No control-thread path waits for a host process: `replace`,
             a start that fails and a protocol fault only signal the kill;
             `retired()` and the destructor let the host go into a list
             reaped from `poll`/`stopped`; a chain destroyed without
             asking first hands its running hosts to
             `HostSupervisor::adopt`, reaped by `reap()` from
             PluginHosting's poll (one bounded grace at teardown only).
             With it the two #183 follow-ups: `ChildProcess::wait` takes
             only POLLIN as an exit (POLLHUP, POLLERR, POLLNVAL fall back
             to polling waitpid for the rest of the bound), and
             `platform_child_exit` skips (77) without a pidfd. ctest
             `plugin_client_host_control_stop_paths`,
             `platform_child_exit` (hangup_is_not_an_exit).
       - [ ] `SupervisedChain::poll_starting` (`engine/plugin_client/src/host_supervisor.cpp` ~1041): a Ready host that `bind()` refuses (no free slot) dies in `~HostProcess::stop()` on the control thread (2 s deaf, unbounded in D state); `let_go(kill)` it instead. `~HostProcess` still calls the blocking `stop()` (`wait(2000)`, then `wait(-1)`): give teardown one bounded SIGKILL+reap and leave a D-state host to init (`~HostSupervisor` adds a second 2.5 s grace on top) (#191 review).
       - [ ] Gestures from an editor still open on a host being retired
             are not taken (#78 note 3 on #80; short-lived).
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
       - [x] One block size, one policy (auditor finding 4, rt-engineer
             item 3): `PluginHosting` is built with `block_frames` = the
             backend's period once known and `max_chain_channels = 2`; a
             period change is fatal to the activation (ADR-0007 §4.2;
             amendment change D), and a reopen builds a new one. Done in the RT PR: Session's
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
       - [x] Re-post when `plugin_params_dropped()` moves (every running
             chain's document values, at most every 100 ms), and
             coalesce-and-retry a refused post, the newest value kept per
             (store, instance, parameter): `PluginHosting`. The re-post
             sends the bypass too (`plugin_hosting_repost_bypass_off`: an
             un-bypass dropped by a plan swap). A coalesced GestureEnd is
             handed over before a newer message of its parameter
             (`plugin_client_host_control_coalesce_order`); a touch burst
             after a stalled poll survives
             (`plugin_client_host_control_stalled_touches`).
       - [ ] After a callback the runner did not take, the next collect
             plays the host's block from before the skipped callback (one
             period late, ramped in): the client's callback count does not
             move on a skipped callback. Rare (period mismatch, a cleared
             runner); a listening check decides whether it needs more.
       - [x] `PluginFault` from the client, not the runner (rt-engineer
             item 2): from each SupervisedChain's state (restarting, down,
             culprit), probes (missing, refused) and the hosts' refusals,
             one standing per insert (`PluginHosting::faults`).
       - [x] One owner for teardown (session, hosts, runner, supervisor,
             client): `PluginHosting`, for one backend activation,
             released in `EngineControlServer::after_deactivate`.
       - [ ] `session_plugins_*` moved to engine/control (the resolver now
             drives `plugin_hosting_*` there; the core-level ones stay
             until then).
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
       - [x] M3, the control server's side: the control thread sleeps on a
             `common/platform` WaitSet (self-pipe, polled on that thread): its
             clients' notify(), each running host's `exit_handle()` and, only
             while `wants_write()`, its socket for POLLOUT, from a registry
             of every host the supervisor spawned. Only what the next turn
             services is listed, so nothing spins. ctest `platform_wait_set`,
             `plugin_client_host_control_watch`.
       - [ ] Hosts' sockets for POLLIN too (host_control.h asks for it): a
             gesture from an editor reaches the document at the next 50 ms
             turn today; needs a service-every-turn rule for every holder
             that drains, so a dying host's unread datagrams cannot spin.
       - [ ] WaitSet spin (#198 review): a chain in `PluginHosting::retiring_`
             awaiting its epoch is never pumped, yet `HostSupervisor::watch`
             (`engine/plugin_client/src/host_supervisor.cpp` ~736) lists its
             hosts' sockets Writable while `wants_write()`; pump those hosts
             each turn (or list only serviced holders) so POLLOUT cannot wake
             the control thread in a loop (`plugin_hosting.cpp` ~865-870: a
             restart sending a multi-MB Load while the chain retires).
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
       - [x] Start and stop hosts from the document diff (the set of active
         insert ids before and after each change), not from the op kind:
         `PluginHosting::show` after every document change (undo, redo,
         SetInsertActive included); AddInsert/ReplaceInsert are resolved against
         the scan worker's catalog (amendment of 2026-10-19, item 6).
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
       - [x] LV2 `worker:schedule` (required or optional): a request and a
         response ring per instance (lock-free, 16 KiB each, a full ring
         answers `LV2_WORKER_ERR_NO_SPACE`), one worker thread per
         processor at the host's normal priority, woken by `signal()` like
         `done_word`; responses and `end_run` inside the processing
         thread's run, bounded by what had arrived when it began; work()
         never beside restore() or re-activation (a lock the processing
         thread never takes). Schedules from restore() are answered in the
         first run. The worker thread has a 512 KiB stack (`start_thread`,
         locked memory), and a plugin whose worker cannot start is refused.
         ctest `plugin_host_worker{,_futex,_control}` (the test plugin
         applies its gain only through work()'s two responses, and goes
         silent if work() runs on the run thread, a response arrives
         anywhere else, or a run() follows one without end_run; fails
         refused without the feature), `plugin_host_worker_restore` (a
         schedule from restore() answered in the first blocks),
         `plugin_host_lv2_worker` (the ring).
       - [x] A work() that hangs no longer holds the host's main thread:
         restore and re-activation take the worker's lock with a deadline
         (the engine's bound for the answer, kLoadTimeoutMs or
         kReconfigureTimeoutMs, less 1 s), and past it are refused with the
         reason (a Reconfigure answered by Fault{Plugin} naming the plugin's
         hung work(), the host exits Faulted). Teardown waits 1 s for the
         worker, then abandons it and `_exit`s Faulted, running no
         destructor that would free what a returning work() touches (its
         instance, the URID map, the library; #111 review). ctest
         `plugin_host_worker_hang` (the test plugin's work() never returns
         at gain 0.25; fails without the change: no answer for 10 s and a
         host that never exits), `plugin_host_worker_slow_exit` (a work()
         that returns and maps a URID 1.5 s after the abandon; the host
         has already exited Faulted, not by a signal).
       - [x] Worker requests and responses refused because a ring was full
         are told to the operator, as ADR-0012 change G treats non-finite
         output: the host sends Fault{WorkRefused} (HostControl wire 2)
         naming the plugin, the first at once and the rest summed at most
         once per second, and the rest after its last run(); the engine
         posts one non-standing Notice per run of the chain naming plugin,
         track and slot, and carries the count in InsertState::work_refused
         (state layout 3, only rises). No fault stands. ctest
         `plugin_host_worker_flood` (the test plugin schedules 1536
         requests a block, 1 ms of work() each, at gain 0.0625),
         `plugin_hosting_work_refused`, `plugin_hosting_server_work_refused`
         (the count in the state block, one Notice at the client, no
         PluginFault); each fails without the change.
       - [x] Touches the engine never got are told to the operator the same
         way (HostControl wire 3): Fault{EventsDropped} now counts only
         touches lost in the host's queue (instance -1, per chain), and
         touches of a parameter with no dense index are
         Fault{ParamUnmapped} naming the plugin. The host throttles all
         three counted faults alike (first at once, then summed at most
         once per second, the rest sent before it stops). The engine counts
         them (`PluginHosting::touches_lost(track)`,
         `param_unmapped(insert)`) and posts one non-standing Notice per run
         of the chain: the track for lost touches, plugin, track and slot
         for unmapped ones. No fault stands. A move whose touches were lost
         is still recorded (#121 review): an end with its begin lost, or at
         a value newer than the one end_all sent, is a gesture of its own,
         and 100 ms after a drop (and before the host stops) every
         parameter and bypass whose plugin value differs from the engine's
         last value, with no gesture open, is sent as one (a lone edit
         lost). ctest `plugin_host_gestures`
         (`an_end_with_its_begin_lost_is_recorded`,
         `a_lost_lone_edit_is_recovered_from_the_plugin`); the counts clear
         on session open (`plugin_hosting_touch_faults`). ctest
         `plugin_hosting_touch_faults`, `plugin_host_vst3_gesture` (the
         read-only touch is ParamUnmapped for instance 0, no
         EventsDropped), `contract_host_control`.
       - [ ] The lost and unmapped touch counts are not in the state block
         (InsertState has `work_refused` only; lost touches are per track):
         a state layout change with the UI's co-review.
       - [x] Plugin state through the session file (`t4-lv2-state`):
         `FlushPluginState` with hosting sends GetState to every running
         instance (`PluginHosting::request_states`; ids unique across
         hosts; a host that stops, restarts or is replaced before it
         answers fails its requests, one silent past kReplyTimeoutMs is
         killed), the saver thread writes `Plugin State/<sha256>.state`
         (`SessionFolder::write_states`, paced behind a checkpoint like a
         save), then `SetInsertState` goes to the document and every
         replica, then the reply; Failed names each insert whose state
         could not be had. A plugin with no state of its own gets none.
         A crashed host restarts from the flushed state
         (`SupervisedChain::set_state`), not the one it started with.
         ctest `plugin_hosting_state_flush` (fails without set_state: 605
         blocks flushed, 4 after the crash), `plugin_hosting_server_state_flush`,
         `session_folder`, `session_saver`. The LV2 host's save/restore
         and `worker:schedule` were already in (`plugin_host_state_lv2`,
         `plugin_host_worker_restore`). No protocol change.
       - [x] (#269) The GetState timeout is tested: ctest
         `plugin_hosting_state_read_timeout` slows the worker plugin's save() past 5 s through
         `SONORA_TEST_SLOW_SAVE_MS`; it fails with the deadline disabled.
         - [ ] (#269 review) It asserts `longest_turn < 100` ms; on the 50-minute sanitizer job a
           loaded runner could stall a turn that long: measure it there or raise the bound to about 500 ms.
       - [x] States go to the engine's session folder, the session file
         wherever the UI saves it: the saver now copies each state the
         file names into its own folder (tmp, fdatasync, rename, one
         `Plugin State/` fsync) before writing it, and refuses the save,
         by name, when one is in neither folder (#131 B1). The folder
         sync is repeated until one has covered every state (N1), and
         `Plugin State/*.state.tmp` is swept when a folder is first seen
         (N2). ctest `session_folder`, `plugin_hosting_server_state_flush`.
       - [ ] Recorded audio has the same root cause (#131 review): takes
         land in the engine's `--dir`, the session file wherever the UI
         saves it, so a session saved elsewhere names audio its folder
         does not hold. Fix at the root: the engine follows the session
         folder (open/save moves `--dir`), which would also retire the
         state copy above.
       - [x] LV2 states that hold files, in the host (ADR-0029 slice 1, #276):
         `makePath`, `mapPath`, `freePath`, HostControl wire 4, ctest `plugins_host_state_files`.
       - [x] (#283, #289) Host side of safe saving: byte caps (1 GiB per file, 4 GiB per save, 512 MiB
         kept free), one State deadline per host at a time, a copy budget against `kStateReplyTimeoutMs`,
         `.incoming-*` sweep, 0444 files re-hashed before use, no write through links.
       - [ ] LV2 states that hold files, in the session (ADR-0029 slice 2): the engine sends
         `scratch_dir`/`session_dir`/`save_dir` and `kGetStateNoCopy` during a take, `StateRef.files`,
         Save As and cleanup. Needs session format 17 and protocol 40 (#290 took 16, 13 and 39);
         shares `plugin_hosting.cpp` with the ADR-0027 lane work, so not in parallel with it.
         - [ ] Engine: wait `kStateReplyTimeoutMs` (60 s) for a `State`, not `kReplyTimeoutMs`.
         - [ ] Engine: show an operator notice for a counted `Fault{Plugin}` (path refusals);
           `plugin_hosting.cpp` ignores it today.
         - [ ] `-Wmissing-field-initializers`: positional brace inits of `GetState`/`State`/`LoadInstance`
           warn (`contracts/plugin/tests/test_host_control.cpp`, `plugin_hosting.cpp:1150`).
       - [ ] plugins.host.state follow-ups from the #276 review:
         - [x] N1 (#283): byte caps per file and per save, free-space check before copying.
         - [x] N2 (#289): sweep `Plugin Files/.incoming-*` left by a host killed mid-copy.
         - [x] N3 (#289): saved plugin files read-only (0444), re-hashed before being named by their sha.
         - [x] N4 (#289): refuse to save through a symlinked `Plugin Files/` or `<sha256>/`.
         - [x] N5 (#289): ADR-0029 §6 amended for `absolute_path`'s passthrough.
         - [x] N6 (#289): makePath through a symlink out of the scratch folder, refused and counted.
         - [x] (#283 review, #289) GetState deadlines of one chain run one at a time; a copy aborts with a
           Failed text near `kStateReplyTimeoutMs`; `plugins_host_state_copy_caps` covers `free_space`
           false, free < size and a source that grows mid-copy.
         - [ ] Measure copy+fdatasync throughput on a real USB 3 stick to justify `kMaxStateCopyBytes` and
           `kStateCopyBudgetMs` (#283 review, #289).
         - [ ] The 512 MiB free-space margin is checked per host and per file; concurrent hosts or a take
           can consume it together; reserve or serialise across hosts in slice 2 (#283 review).
         - [ ] Verify `Plugin Files/` against its directory hashes when a received session is opened
           (engine, slice 2): a file the host never saw is trusted at restore (#289).
         - [ ] "Collect plugin files": an operator action that copies files named by pre-wire-4 absolute
           paths into the session (ADR-0029 §6 amendment, #289).
         - [ ] Open plugin files with `O_NONBLOCK` in `copy_in`/`name_in` so a FIFO swapped in after the
           `is_regular_file`/`lstat` check cannot block the save until the engine kills the host (#289 review N1).
         - [ ] `copy_in` keeps an existing `Plugin Files/<sha>/<name>` that verifies instead of renaming
           over it; the new inode makes other hosts' `seen_` entries stale ("saved after the take") (#289 review N2).
         - [ ] Check whether 0444 plugin files on a FAT/exFAT session disk get the read-only attribute and
           block deleting the session folder on Windows (suspected, #289 review N3).
         - [ ] ADR-0029 §6: limit the "no absolute path from a restored state is copied" guarantee to paths
           passed through `absolute_path`; a path kept in a plugin's opaque state is not covered (#289 re-review N1).
         - [ ] Probe: give `--probe` a scratch folder, so a plugin that requires `state:makePath` can be
           scanned as usable.
         - [ ] Copy plugin files with reflink (`FICLONE`) where the filesystem supports it.
         - [ ] lilv prints three warning lines to the host's stderr for each saved `atom:Path`: filter
           them, or pass a base URI, once host stderr reaches the operator's log.
         - [ ] Slice 5: a `GetState` whose `save_dir` is a preset store also becomes the host's session
           folder (ADR §6 as written); presets should get a flag, or the host should not move its session folder.
       - [ ] `SetInsertParams` for editor values moved mid-gesture at flush
         time: no editors are hosted yet, so nothing is moved outside a
         gesture end; do it with editors under WSLg.
       - [ ] Presets: LV2 `pset:Preset` listing and load (lilv), VST3
         `.vstpreset` (published SDK format), user presets saved as
         StateBlobs beside the catalog cache; a load is a make-before-break
         restart with the preset as the instance's state. ADR-0029 (#270)
         sets the slices: user presets live in `$XDG_DATA_HOME/sonora/presets/`, not the cache.
         - [x] Slice 1 (host files, #276, above) and slice 4 (user preset store,
           `preset_store.{h,cpp}`, `sonora_preset 1`, #275).
         - [ ] Slice 2: plugin files in the session (above).
         - [x] Slice 3: factory preset listing (LV2 `pset:Preset`, VST3 programs, `.vstpreset`) in the
           probe record and `--read-preset` in the host (#278); `FactoryPresetReader`.
         - [ ] (#278) Wire `FactoryPresetReader` into the scan worker and `LoadPreset` (slice 5); call
           `forget_blocked()` on `plugin.rescan`; read the `.vstpreset` `Info` chunk (category, name) for the list.
         - [ ] (#278) A VST3 program-change parameter's value in the document is written at every Load;
           leave `kIsProgramChange` parameters out of the initial values (`plugin_hosting`).
         - [ ] (#278) Unverified on real plugins: Surge XT and a yabridge plugin, on hardware or in an image that has them.
         - [ ] (#278 review N2) LV2 restore: surface the plugin's `restore()` status (call the state
           interface directly, or wrap it) so `--read-preset` and Load refuse a state the plugin rejects
           (`lilv_state_restore` returns void; the session-restore path has the same gap).
         - [ ] (#278 review N3) `FactoryPresetReader`: persist Blocked presets with the catalog cache
           (cleared on rescan or bundle change), as probes are, when slice 5 wires it.
         - [ ] (#278 review N4) LV2 presets: stat-cap a preset's `seeAlso` files before
           `lilv_world_load_resource`, so one oversized pset .ttl cannot time out the plugin's probe.
         - [ ] Slice 5: the `LoadInsertPreset` op through `engine_control_server.cpp`.
         - [ ] Slice 6: the preset menu in the insert panel (ui-engineer).
         - [ ] Slice 7: mix undo for a preset load (Track 2 item 6).
         - [ ] preset_store follow-ups (#275 review): fsync the parent of every folder `save` creates
           (`<key>` in the root, `<sha>` in Plugin Files); refuse to read through a symlinked plugin,
           Plugin State or `Plugin Files/<sha>` folder in `load`, not only in cleanup and `save`; `save`
           returns IoError when only the post-rename directory sync fails although the preset exists
           (report saved-unsynced); reject Windows-reserved basenames (CON, NUL, COM1, trailing dot or
           space) before the Windows port.
         - [ ] (#275 re-review N1) `PresetStore::save` checks `real_dir` on each store folder before
           `make_dir`, so a linked Plugin Files or plugin folder never gets an empty folder created in its target.
         - [ ] (#275 re-review N2) `PresetStore::remove` and `rename` refuse a linked plugin folder
           (`real_dir` check, as `save` does).
       - [ ] Plugin manager (ui/qt): the catalog with refusals, rescan,
         per-plugin block list, the yabridge bundles; screenshots.
6. [ ] **Windows VST through yabridge** (in progress, plugin-engineer;
       ADR-0017, Proposed).
       - [x] Discovery, validation and the real bridge measured (PR 1).
         yabridge 5.1.1 pinned, on Debian's Wine 10.0, with both licences
         recorded (ADR-0017 Y1). The host marks `"bridge":"yabridge"` on
         `--list` and `--bundles-only` lines. The rule is the chainloader
         in the Linux binary, not the Windows directory alone (Y2). A
         `moduleinfo.json` class ID now matches in either byte order:
         before, a bridged bundle with a COM-order Resources file was "not
         installed" (Y3). ctest `plugin_host_yabridge_list_bundles`,
         `plugin_host_yabridge_list`, `plugin_host_yabridge_probe_com_order`
         (fixtures, no Wine; the last fails without Y3) and
         `plugin_host_yabridge_wine`. The Wine test is skipped (77) in CI
         and runs under `tools/test-yabridge.sh`: Sonora's test plugin is
         cross-built for Windows, and the test covers scan, probe, a
         3000-block run, a factory crash costing one entry, and the Liar
         refused by the timeout (Y4). Container round trip: p50 161 µs and
         p99 598 µs through yabridge, against 58 µs and 250 µs native.
       - [ ] Follow-ups (ADR-0017 Y2, Y4, Y6; update of 2026-10-25):
         - [x] `bridge` in the probe record (version 2, version 1 still
           read) and in `contracts/api` PluginInfo (protocol 14), so the
           operator sees "Windows (Wine)" in the chooser and the insert
           panel. ctest `plugin_probe_record`, `api_wire`, `ui_inserts`,
           `plugin_host_yabridge_probe_com_order`; the conformance check
           (`api_conformance_fake` and the engine's) holds that only a
           VST3 is bridged.
         - [x] A crash during a running chain under Wine:
           `yabridge_wine.sh` step 7 (`plugin_driver --scenario
           crash_bridged`), in the Wine-only run (`tools/test-yabridge.sh`;
           skipped, 77, in CI). The host dying and the host going silent
           are both accepted, as long as the callback never waits and the
           plugin is named. Which one Wine gives is not yet measured.
         - [x] The Wine prefix created before the first scan, so a cold probe
           is not cached Blocked: the scanner runs `wineboot --init` before
           anything bridged when `<prefix>/system.reg` is missing. A failure
           is said and nothing bridged is cached. ctest
           `plugin_scanner` (`wine_prefix`, fixtures, no Wine; the cold
           probe is cached Blocked without it) and `plugin_paths`.
         - [ ] The deadline sign-off on the appliance with the UMC1820 and a
           real Windows plugin, before it is offered.
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
8. [ ] Follow-ups from the PR #80 review (non-blocking):
       - `plugin.retry` is unhandled, so the fault text should name the
         recovery steps (make the insert inactive and then active, remove it,
         or reload the session). Add a test that a later document change
         leaves a down chain down.
       - The forced re-post overwrites a parameter that is being dragged; it
         should use the last touched value.
       - [x] A saved state that cannot be restored (its file over the 16 MiB
         state cap, missing, unreadable, not matching its hash, or refused
         by the host in `Loaded`) loads at defaults plus its params and
         raises a standing `PluginFault` on that insert, cleared once the
         document names another state or none (ctest
         `plugin_hosting_state_not_restored`).
- [ ] Plugin parameter automation (ADR-0027 §5): lanes keyed by insert id
      and ParamValue key, carried in the plan beside the mix lanes,
      evaluated once per block at the block's first frame less the chain's
      latency into ADR-0012's block-tagged last-value table; no new IPC.
      After the write slice (Track 1).
  - [x] Slice 1, engine side (#212): lanes are played once per callback into
        the chain. The parent item stays open for the rest.
- [x] `insert_chain_runner.h:38-41` comment says events are posted
      changes followed by plan plugin lane values (#243).
- [x] `test_plugin_automation`: Trim mode, a lane on an unbound or
      silent chain, and a period of another size (#243).
- [x] Engine never sends a non-finite plugin lane value, so a NaN lane is no longer
      resent every callback (#243); the model already refused them (#226).
  - [ ] Track 1/2: the plan builder refuses non-finite plugin lane values when it gains
        plugin lanes (with key-to-index resolution).
  - [ ] #243 review: a skipped non-finite lane lets a posted knob win; interpolate in
        double or clamp so an overflowing pair of finite points still yields a value.
- [x] (#226) Plugin lanes in the model: `AutomationParam::Plugin` keyed by insert id and
      ParamValue key, session format 14, window format 11, protocol 36; lanes follow their
      insert (remove, replace, move, duplicate, undo). Saved, undone and replicated.
- [x] (#250) Plugin lanes: builder key-to-index resolution; a lane whose key names no
      parameter of the plugin is skipped (`LaneResolver`, `PluginHosting::resolve_lane`;
      over `max_plugin_lanes` the build is refused).
- [ ] Plugin lanes follow-ups from #212 and #226: restoring knob values on leaving lane
      play (post to the first plan without the lane; shares Touch release with the write
      slice); per-insert latency; touch and write; mute and send automation; UI.
  - [ ] #250 review: a scenario with two inserts automating a parameter of the second
        instance.
  - [ ] #250 review: stepped, integer and enum parameters carried as a flag in `ParamDesc`,
        so a lane holds between points instead of interpolating.

### Track 5: dev environment (distro-engineer, researcher)

1. [ ] Measure WSLg audio from WSL2 Debian trixie (`devenv.wsl_measure`). Done (#292, first real run
       on the laptop, `brain/research/wsl-dev-environment-measurements.md`): playback bridge works,
       quantum 48000/128 stable, engine xruns=0 over about 4 min. Still missing:
   - [ ] the 10-minute xrun run;
   - [ ] round-trip latency (no tool yet);
   - [ ] the microphone (silent at RDPSource: check with a real mic);
   - [ ] WSLg's PulseAudio server stops answering while the mic tunnel is linked (seen 3 times,
         `rdp-source ... q overrun`; `wsl --terminate Debian` cures it): try a newer WSLg
         (`wsl --update`) or open the mic tunnel only while recording (#292).
2. [x] `tools/dev-wsl.sh`: builds engine and UI, starts PipeWire on onboard audio, runs both. (#284, #292:
       first real run, systemd socket released, stalled-tunnel detection, default flow exits 0.)
   - [ ] (#292 review) `tunnel_dead` requires the overrun line to be recent (or a count of at least 3 in
         the last 20 lines) so one recovered overrun cannot restart PipeWire; do not restart PipeWire
         from `selftest` while the engine runs.
   - [ ] (#292 review) `dev-wsl.ps1`: map exit 143 to 0 only when this script ran `stop` (a marker file
         written by `do_stop`), not for any SIGTERM of the UI; the mapping is verified by reading only.
   - [ ] (#292 review) Mark failure 3 (143 to 0) in `wsl-dev-environment-measurements.md` as verified by reading only.
   - [ ] (#284 review) `dev-wsl.sh user`: back up `/etc/wsl.conf` to `wsl.conf.sonora-bak` on the first change;
         warn when uid 1000 is another user (NOPASSWD sudo is granted); `wsl_conf_set_user` must strip CR
         and comments before matching `[user]`; remove the duplicate old `90-NAME` sudoers file.
   - [ ] (#284 review) CI "Shell scripts" step: fold `apt-get install dbus-x11` into the Install step
         (it relies on the earlier `apt-get update`).
3. [ ] CI: the Thread and address sanitizers job ran out of disk (No space left
       on device) in the ASan/UBSan build on a docs-only PR (run 41246994280);
       add a free-disk-space step or prune the build tree between the TSan and
       ASan builds.
4. [ ] CI follow-ups from #230: `tools/merge-checks.sh` takes its missing-set snapshot before
       it reads the head SHA, so a push in that narrow gap with only cheap checks registered
       could pass; recompute the missing set after the head read. Measure the ccache hit rate
       and the new sanitizer duration over two runs after #230 (the first is cold). Trade-offs
       #230 left unapplied: move ASan+UBSan of the dsp_/native_ suites to nightly (largest
       saving, UB in a processor caught a day late); split Thread and address sanitizers into
       two parallel jobs; merge Record, BWF interop and Quantum into one job (both rename
       required checks).
5. [x] `ci.yml` hunks of #242 and #247 (merged).
6. [x] #223 review: `bus_dead` checks the saved bus pid (#284); `sonora-wsl.cmd` has no paren block and
       exits with the script's code (#284). `status`, default flow, `user`, `run`, `shortcut` ran on the
       laptop (#292).
7. [x] (#251) `WavWriter` counts only frames that reached the file; a failed flush never
       leaves a header claiming audio the disk refused (unbuffered stream, `close()` clamps
       to the file).
   - [ ] #246/#251: delete `KNOWN_SHORT`, `MAX_KNOWN_SHORT_BYTES` and their branch in
         `check_take()` from `tools/failed-take-quit.py`, and the `self_test` cases that
         expect `known=True`, now that #251 fixed the miscount. OS 2.
   - [ ] #251 review: build the 654-byte header in memory and write it with one `fwrite`;
         run the refused-take path on MSVC (`setvbuf` and the `SEEK_END` pad seek are untested there).
   - [ ] Waits on the owner's authorization: delete the old git worktrees, 269 safe local branches and 53
         safe remote branches; also the stale remote branch `plugins/state-files-safety` (#289 landed
         the work under another name).
8. [x] (#273) `tools/clean.sh --worktrees-only`: judges every worktree under `.claude/worktrees/` in
       parallel, then removes the merged, clean, idle ones; `--dry-run` lists the candidates.
   - [ ] `tools/clean.sh --worktrees-only` does not cover the old worktrees outside `.claude/worktrees/`
         (`C:/source/sound-*` and `C:/source/wt-*`, about 6 GB; 25 were removed by hand): extend it to them with the same safety
         rules (not locked, clean status, idle for `ACTIVE_MINUTES`, merged into origin/main, never
         the primary checkout, never a branch delete).
   - [ ] (#273 review) Re-check lock, status and the active-minutes mtime right before each worktree
         remove; judging all first widens the check-to-remove gap to minutes.
   - [ ] (#273 review) Refuse `--worktrees-only` combined with `--deep` or `--host-build-cache` (ignored
         silently today, and the dry-run SUMMARY claims a host cache prune).
   - [ ] (#273 review) Count only successful removals in the SUMMARY "from N worktree(s)"; `WT_COUNT`
         goes up before `act`, so a failed removal is counted.
   - [ ] (#273 review) Pass candidates between the judge jobs and the parent with a separator a path
         cannot hold (tab or NUL), not `|` (`w="${c%%|*}"` cuts a path at its first `|`).
   - [ ] (#273 review) Report a judge job that exited non-zero as "keep (could not judge)" and
         trap-remove the temp dir on exit (a bare `wait` reaps a failed job silently; an abort through
         `wait -n` leaves `$JDIR`).

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

- [x] The image writes `BUILD_ID` into `/etc/os-release` (#207): the plugin
      catalog cache's header carries it (ADR-0012, amendment of 2026-10-19,
      change B); every cache is discarded once, with one Notice, when it
      first appears.
- [x] (#217) The `0100-sonora-system` hook falls back to `BUILD_ID=unstamped` when
      the sanitised id is empty.
- [x] Compile the engine on Debian trixie and fix what the compiler finds. (#240) GCC
      14.2: one `-Wformat-truncation` in `utc_stamp` fixed.
  - [ ] Model track: add `data_from` (or `{}`) to the `PlaceMidiRegion` brace-inits in
        `test_midi.cpp`, `test_midi_chunks.cpp` and `test_sequence_file.cpp` (27 sites,
        `-Wmissing-field-initializers`).
  - [ ] `tools/build-engine.sh` passes `SONORA_DSP_QUICK` and `-j` through to the
        container ctest, as CI does (the local run took about 22 min). Optional.
- [x] Run `ctest`; make the ring buffer and BWF tests pass. (#240) 276/276 in the
      trixie container.
- [x] `live-build` config: package list, RT kernel, PipeWire from backports. (#238)
      PipeWire 1.6.9 and WirePlumber 0.5.12 pinned from a dated backports snapshot; the
      hook fails the build if the pins are not what got installed.
  - [x] (#285) http vs https for the snapshot decided: http (ADR-0002 amendment).
  - [ ] Put trixie and trixie-security on a snapshot too (`lb config --mirror-*`), so a
        point release cannot change the kernel between two builds of one commit: apply the snapshot
        mirrors in `iso/auto/config` plus `config/apt` Check-Valid-Until, with a timed cold build
        through snapshot.debian.org; have `test-image-config.sh` check the timestamp in auto/config
        matches the backports list.
  - [x] (#293) Build the ISO and boot it in QEMU with the pins (`tools/build-iso.sh`,
        `tools/test-boot-qemu.sh`): 978 MB image, BOOT TEST PASS 31 ok, no VT login prompts; stage-only
        engine and UI builds, BUILD_ID stamped on Windows, real linger, QEMU in a container
        (`brain/research/iso-first-build-and-qemu-boot.md`).
    - [ ] (#293) Boot through the bootloader (the test boots `-kernel`/`-initrd`), an unaccelerated TCG boot,
          and CI for these changes are unverified.
    - [ ] (#293) A smoketest check that BUILD_ID is not `unstamped` on a git build (it was only an info line).
    - [ ] (#293 review) `tools/build-iso.sh`: build the staged engine in the trixie container even when the
          host has cmake+JACK (the native path uses the host compiler, e.g. Ubuntu GCC with
          `-march=x86-64-v3`: SIGILL risk on a baseline CPU); stage from the build dir `build-engine.sh`
          just built, not the last existing of `build/` and `build-container/`; add an opt-in
          `SONORA_ISO_TESTS=1` to run the full ctest before staging.
  - [x] (#285) `tools/test-image-config.sh` fails if `getty@tty2..6` is enabled.
  - [ ] (#285 review) `SuccessAction=poweroff-force` (honoured since #285) pre-empts the smoketest script's
        orderly poweroff; use `SuccessAction=poweroff`, or drop the script's own poweroff, and confirm
        `tools/test-write-usb.sh` boot 3 `CLEAN_SHUTDOWN` still passes (no QEMU run has confirmed it).
  - [ ] (#285 review) `tools/test-display-fallback.sh`: `trap EXIT` to `docker rm -f` the boot container and
        label containers `org.sonora=1`.
  - [ ] (#285) Add `sonora-smoketest` to CI's shellcheck list (`-s sh`) after fixing its SC2016/SC2013 infos.
  - [x] (#242) `tools/test-image-config.sh` runs in the CI "Shell scripts" job.
- [x] `sonora-session.service`: autologin on tty1 into the engine, respawn on
      exit, no display manager. (#238) Closes the VT getty gap: `NAutoVTs=0`,
      `ReserveVT=0`.
- [x] PipeWire drop-in pinning quantum to 128 and rate to 48000, and holding
      it against clients: `tools/test-quantum-pin.sh`, in CI.
- [ ] Repeat `tools/test-quantum-pin.sh`'s routes with the UMC1820 as the
      graph driver instead of the dummy driver. A real ALSA node may have
      period-size constraints the container cannot show.
- [x] (#253) Decide whether to ship `default.clock.quantum-limit = 128`: not shipped yet
      (ADR-0028).
- [ ] Run `tools/test-quantum-pin.sh` with and without `quantum-limit = 128` on the UMC1820 and
      on a device that cannot run at 48 kHz; ship it if both are clean (reopens ADR-0028).
      Waits on hardware.
- [x] (#216) `limits.d` granting `rtprio 95` and `memlock unlimited` to the audio
      group. Already shipped; `tools/test-rt-limits.sh` keeps limits.d, the unit and the hook in step.
- [x] (#235) Udev rule naming the first class-compliant USB interface: not needed. Port
      auto-connection uses the node description, not the card index
      (`brain/research/multichannel-usb-interfaces.md`).
- [x] (#253) Two USB devices with the same node description: split into "<desc>", "<desc> (2)",
      recognising both pipewire-jack suffix forms (adapter-node port suffix, renamed client);
      `drivers/fake` injection; container checks in `tools/test-engine-entrypoint.sh`.
- [ ] Verify on two identical USB interfaces: the suffix form on real ALSA nodes, and whether
      they report a usable serial (which would make "(2)" stable across boots). Waits on
      hardware. driver-engineer.
  - [ ] #253 review: reconnect when one of two same-model units is unplugged before connect
        (`jack_backend.cpp:479`); the twin block in `tools/test-engine-entrypoint.sh` greps
        `^twin-$n:capture_` but the client is named "Twin Pro"; log the chosen unit at startup;
        the DeviceGone reason names the raw client part, not "(2)".
- [x] `tools/write-usb.sh`: write the ISO and create the persistence partition.
      Verified against a loop device and booted in QEMU, with persistence
      surviving a reboot: `tools/test-write-usb.sh`. Not yet run on a
      physical stick.
- [ ] (owner step) Write the physical stick with `tools/write-usb.ps1` (#294, open at this chore) using the
      exact command lines in `iso/README.md`. Original item: write a physical USB stick with `tools/write-usb.sh`, boot it on a
      physical machine, BIOS and UEFI, and confirm a session survives a
      reboot. The first time the script's USB/removable check meets real
      hardware.
- [ ] Run `tools/test-write-usb.sh` in CI. The container half
      (`WRITE_USB_BOOT=0`) needs only Docker and the ISO; the boots need QEMU
      with KVM.
- [ ] (#294 in review, open at this chore; N7: the privileged container exposes host disks) A verified way to write the stick from Windows, where the owner works.
      The first attempt (`write-usb-windows.ps1`, removed 2026-10-12) opened
      `\.\PhysicalDrive` with one leading backslash, which .NET resolves to
      `C:\PhysicalDrive`; it erased the stick and then wrote nothing. Check any
      Windows device path byte for byte: a doubled backslash sent through the
      agent's shell tool can arrive as a single one.
- [ ] Boot test on real hardware; record the xrun count in
      `brain/research/` (the first QEMU boot is recorded: `iso-first-build-and-qemu-boot.md`).
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

- [x] (#246, #247) A pty-driven check that a quit during a failed take exits 1 with the
      terminal restored: `tools/failed-take-quit.py`, in the CI `engine` job.

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

- [x] Stage 1, shipped boot unchanged (#208): package block (`libpam-systemd labwc
      xwayland qt6-wayland` + Qt runtime), `/usr/share/sonora/labwc/`,
      `/etc/pam.d/sonora-ui`, `sonora-compositor`, `sonora-wait-for-display`,
      compositor/UI/fallback units behind `sonora.gui`. distro-engineer.
- [x] Stage 1, UI side (#208): `app_id` `sonora-ui`, no fullscreen request, a
      first-frame log line, a documented "unfit display" exit code. ui-engineer.
- [ ] Stage 1 UI, later: `sd_notify` watchdog through `common/platform`. ui-engineer.
- [ ] Stage 2, after M7: `sonora-session.service` becomes
      `sonora-engine.service` + `sonora-tui.service` (tty2); compositor
      enabled by default; smoke test kills labwc and the UI mid-take and
      asserts the engine PID and a growing take file. distro-engineer.
- [ ] Stage 1 QEMU checks (the list above) and the rc.xml mouse contexts
      against labwc 0.8.3: not run yet. distro-engineer.
- [x] `sonora-display-fallback` ran `chvt 2`, which reaches no terminal UI until stage 2
      adds `sonora-tui.service` on tty2. (#241) The fallback now starts
      `sonora-session.service` on tty1 (flag file `/run/sonora-display-failed`).
  - [x] (#285) #241 review: `-` on the fallback's `systemctl stop`; ADR-0010 and `iso/README.md` describe the hand-back.
  - [ ] A QEMU boot with `sonora.gui sonora.expect_fallback` and a `sonora-ui` that exits 3
        (`tools/test-boot-qemu.sh`), then asserts `sonora-session.service` is active on tty1. The systemd
        half is in CI (#285) and the smoketest branch is ready, but nothing has run it. distro-engineer.
- [x] (#217) `sonora-smoketest` checks `BUILD_ID` and the `sonora.gui` path.
- [x] (#217) `tools/build-iso.sh` stages `sonora-ui` when `SONORA_WITH_UI=1`.
- [x] Qt runtime packages are not in the image package list yet (stage 2). (#241) The
      block (`qt6-wayland libqt6quick6 qml6-module-qtquick`) is derived from what
      `ui/qt` uses and checked by an offscreen run on bare trixie.
- [x] #217 review: `tools/build-iso.sh` sent `build-ui.sh` output to /dev/null so a
      failure was silent. (#241) Engine and UI builds now log to `iso/build-*.log`.
- [ ] #217 review: the smoketest's `sonora-ui.service` "skipped" check passes if
      the unit is missing; the `sonora.gui` branch does not assert `sonora-ui` active.
      distro-engineer.
- [x] (#217) `main.cpp` comments about tty2 and the terminal UI now say it holds only from
      stage 2, and the over-long line is shortened.
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
- [x] P6, with M5 (#211): `SONORA_PLATFORM` (linux, neutral, and windows, which
      fails at configure for now) and `SONORA_BACKENDS`. Compiler flags go
      under generator expressions. distro-engineer.
- [ ] P6 remainder: add the neutral-module include allowlist, a platform-macro
      check and an OS-path check to M5's script. distro-engineer.
- [ ] P7, right after M5 and before M6: a required `portable` CI job on
      `windows-latest` with MSVC, building `SONORA_PLATFORM=neutral` and
      running `ctest`. Record the first 10 run durations in `brain/research/`.
      distro-engineer.
- [ ] P7 stays open until the portable job is green and required and its
      first 10 run durations are recorded. Widen its ctest filter to
      `dsp_|native_|json|measure_` (`test_dsp_transformer` is in the Build step since
      #222; the filter waits for the job to be proven).
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
- [x] `transport.locate` with the frame in `argument` (a click on the ruler)
      (done in #123).
- [x] State block layout 5 (#213): "Locating...", the monitor bus peaks and
      the playback miss count. Contract PRs of their own (ADR-0009 update,
      item 4, "Not in this port").
- [x] (#220) Bus meters back in sonora-ui (Qt, in the Mix window and the Transport
      bar, with screenshots in the PR), from the layout 5 bus peaks.
- [ ] Bus meters in sonora-tui, from the layout 5 bus peaks. ui-engineer with
      rt-engineer.
- [x] Input monitoring with latency compensation measured by a loopback test
      (ADR-0019, `t1-input-monitor`): `LatencyProbe` in the callback, takes
      captured L later on the playhead and labelled P0 − L, drivers/fake's
      loopback cable in `test_latency_probe`.
- [x] Its follow-ups (ADR-0019 update of 2026-10-26, `t1-latency-wire`,
      protocol 22): latency.measure, latency.cancel and latency.set on the
      wire with a standing Latency event; L kept per capture device, rate
      and period in `$XDG_CONFIG_HOME/sonora/latency.conf`; at start
      --input-latency, then --measure-latency, then the kept figure, then
      the backend's reported round trip (JACK port latencies); the Qt
      Latency window under Setup with refusals held in it.
- [x] t1 latency: reset LatencyProbe::pings_heard_ in start() (the audio
      thread is idle then) so a second measurement does not announce the
      last run's "3 of 3 pings" before "0 of 3" (#152 N1)
- [x] t1 latency: refuse transport.record (and toggle_record, punch) while
      a latency measurement runs, so no take starts with the probe
      overwriting an output; FakeEngine and conformance match (#152 N2)
- [x] t1 latency: re-read AudioBackend::reported_latency() on the control
      thread while the source is None, so the backend fallback is not lost
      to connection order at start (#152 N3)
- [x] t1 latency: fix the Options::latency_file comment; an
      --input-latency run still keeps a later latency.set or measurement
      (#152 N4)
- [x] t1 control test flake: `late_greeting()` in test_control could see
      no Latency event (test_control.cpp:1983, once locally and once under
      TSan in CI): the standing events wait behind the new client's
      Snapshot, which another thread encodes, so one round trip did not
      prove them sent. It now waits for the catalog and makes a second
      round trip.
- [ ] t1 latency: skip follow_reported_latency() once audio_->fatal(), so the control thread stops querying JACK ports after the server is gone (#153 review)
- [x] t1 heap counter: every glibc allocator override in playback_test.h is
      noexcept, so a TU that includes <malloc.h> (or <random> on x86-64-v3,
      via <mm_malloc.h>) after it still builds; test_heap_counter builds
      that order and checks each allocator is counted (#154)
- [ ] t1 engine arithmetic: decide whether sonora_core builds with
      -ffp-contract=off like sonora_dsp (ADR-0011 §2.3). Today the mix is
      deterministic run to run, but its bits depend on -march: Ubuntu
      26.04's amd64v3 GCC defaults to x86-64-v3 and fuses a + b * c into
      FMAs that a baseline x86-64 or another arch build does not (#154)
- [ ] t1 heap counter: test_heap_counter asserts exact operator new/delete counts, not just !heap_untouched() (#154 review)
- [ ] Transport locate without dropping a block.
- [x] Flake explained: the loopback PLAYBACK check's swapped blocks were a
      period PipeWire's dummy driver skipped without an xrun, which the
      self-linked input reads as two blocks swapped; the engine's output was
      continuous. The skip is now a clock step, and only then may a block
      null a period early or late. Postmortem
      2026-10-13-loopback-swap-skipped-period.
- [ ] Locate latency as ADR-0009 §4.4 asks: p99 and max per session in
      diagnostics ("Locating…" on screen past 100 ms is done, #220).
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
      (a) and (b) landed in PR #91; (c) and (d) on `feature/transport-punch`:
      failures are kept per take and said for the newest take only, so take
      K's is said when its close is collected and no longer as K + 1's
      (test_playback_control test_failure_does_not_follow_into_the_next_take).
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

- [ ] Four screenshots went stale in earlier PRs and need their own refresh PR (ui-engineer; open each
  and list them): `docs/images/sonora-ui-edit-clipboard.png`, `sonora-ui-edit-fill.png`,
  `sonora-ui-edit-multitrack.png` and `sonora-ui-transport.png`. Also the old
  `sonora-ui-refusal.png` / `sonora-ui-refusal-reconnect.png` pair still shows protocol 32/31.
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

- [x] N0: `common/dsp` skeleton (neutral, `arch/generic` + `arch/x86_64`
      for flush-to-zero), `plugins/native/sdk` (`Processor`, parameter table,
      descriptor and preset codecs on `common/json`) (#34). dsp-engineer.
- [ ] N0 skeleton remainder: the new edges in M5's check. dsp-engineer.
- [x] N0: `sonora-dsp-measure` harness and golden-file infrastructure:
      response, THD, step timing, aliasing, latency equals reported,
      denormal, non-finite, zipper; RTSan job runs it (#34). dsp-engineer.
- [x] N0: BS.1770 loudness and true-peak in `common/dsp`, checked against
      EBU Tech 3341/3342 cases (#34). dsp-engineer.
- [x] N0: EQ, gate, compressor, limiter, delay, chorus as `Processor`s (#34, #40, #43),
      meeting their `quality.json`. CPU figures on the owner's laptop
      recorded in `brain/research/` (ADR-0011 Unverified 4). dsp-engineer.
- [ ] Still missing after N0 (PR #34 review, 2026-10-13): ~~the RTSan job
      that runs the harness and the golden tests~~ (done in `ci/rtsan`:
      native_* and dsp_* run under RTSan in CI job `rtsan`), and the MSVC `portable` job that runs the
      golden tests under MSVC (ADR-0011 §3.4, ADR-0008 P7 above). Until
      both exist, "RTSan-clean" and "passes under MSVC" are claims nobody
      has checked for `common/dsp` and `plugins/native`. distro-engineer
      with dsp-engineer.
- [x] Research, before N1 (#209, `brain/research/plugin-licences-onnx.md`): licences of DPF (pinned commit), the VST3 SDK
      (MIT since 3.8, reported), ONNX Runtime; ONNX Runtime on trixie and
      its installed size (ADR-0011 Unverified 1, 3). researcher; feeds
      `brain/research/native-plugin-suite.md`.
- [ ] Licence follow-ups from `brain/research/plugin-licences-onnx.md` (#209):
      an LGPL scan of ONNX Runtime's ThirdPartyNotices, its install size with
      its dependencies, MSVC and arm64 builds, where third-party notices ship,
      and EBU Tech 3341/3342 test-signal redistribution (the N0 BS.1770 item).
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
- [x] (#224) N1, with M9: how the engine routes a key to a plugin's side-chain.
      ADR-0012 amendment of 2026-11-01: the key is the region's last input planes,
      layout version stays 1; `InsertChainRef::key_channels`, and `process_chain`
      takes the key planes as their own argument. plugin-engineer + rt-engineer.
- [ ] N1 side-chain key follow-ups from #224 (bump the session format): the model
      field for the key source (an aux bus's sum before its return fader, a second input
      edge of the InsertChain node so PDC aligns it; a key depending on the keyed
      track's own output is refused as a loop), the key edge in `plan_builder`, and the
      supervisor spawning a chain with `key_channels` set from it. Until then
      `engine/core`'s chain runner builds unkeyed refs, so a keyed slot is a counted
      shape fault (silence). plugin-engineer + rt-engineer.
- [x] (#249) N1 follow-up (PR #40 RT audit, low): DPF's `lv2_set_options` calls
      `setBufferSize(..., true)`, which deactivates and re-activates, and so
      re-prepares and allocates, on whatever thread the host calls
      `options:set` from. Check the LV2 options interface's threading class
      and what Ardour, Carla and our host do. If a host can call it beside
      `run()`, defer the change to the next `activate()`. dsp-engineer.
      Done: `options` is instantiation-class, Ardour and Carla call it off the audio thread, ours
      never; kept immediate (deferring would move the allocation into `run()` on a lazy
      activate); held by `native_format_options_lv2_*`.
  - [ ] #249 review: add a state-dependent input (a decaying tail) to the options sub-check so
        dropping `reset()` on the same-configuration path fails; the alloc counter sees
        `operator new` only.
  - [ ] #249: a VST3 counterpart: `setupProcessing` with a new rate or block size between two
        `process()` calls on an active component, held to a fresh instance.
- [ ] N1, with M9: plugin-parameter target on `ParameterChange` and the
      read-only telemetry region in `contracts/plugin`. plugin-engineer +
      rt-engineer + ui-engineer.
- [x] (#257) N1: descriptor-driven Qt editor panels in `sonora-ui`: `Knob.qml`, toggle and choices row from the catalog's `api::PluginParam`, no per-plugin QML. ui-engineer.
  - [ ] Plugin knob lane awareness: a knob under a Read/Touch/Latch/Trim lane lies now that #250 plays lanes: show the lane value at the playhead, mark the knob automated, hold a notice on a move (ADR-0027 §2). ui-engineer.
  - [ ] `Knob.qml`: keyboard control and `Accessible` roles; `param_origin()`/`param_position()` return NaN for an infinite min or max (#257 review).
  - [ ] Meters in the generic editor wait for the telemetry region (the line above).
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
- [x] N2 slice 3 (#214): transformer (core saturation and hysteresis, LF first)
      as an output stage of the tube drive, with the supply sag an output
      stage draws (slice 2 has one preamp stage, which draws too little
      for sag to matter). dsp-engineer.
- [x] (#219) N2 slice 3 follow-up: `OutputStage::tick` clamps |u| before u^4; cheaper
      solve (one exp, warm-started Newton).
- [ ] N2 slice 3 follow-ups from #219: (a) class B is not met with the stage on at eco
      (about 106 us calibrated against 60 us), standard or high; needs a tabulated core
      at the base rate (`brain/research/native-tube-n2.md`); until then the stage-on
      path ships only beside an eco mode that meets the class (ADR-0011 §3.5). (b) The research note and the
      comment in `tick` get the old failure wrong: x = 1e100 or 1e300 collapsed vs to 0
      (u^4 overflowed), NaN came only above about 7.2e305. (c) The extremes test does not
      assert the output stays near the rail during the huge input. (d) The note says eco
      at stage 0 % still meets class B, but that run's kernel gate failed (1.79 > 1.76):
      rerun and record. dsp-engineer.
      Update (#258): (b), (c), (d) done and the stage runs once per step at the base rate; class B with the stage on is still
      NOT met (1.81 kernels hot against 1.76). The gate stays.
  - [ ] Transformer stage-on eco CPU gate (#258 review): #288 shortened the core step's dependency chain
        (0.57-0.69 of the time per block) but kept the gate. Record the stage-on worst case from a nightly
        on the EPYC runner, then hold eco stage-on to the class bar plus that figure and lift the gate in
        `cpu_qualities`; if the EPYC figure is over 1.76 the next cut is algebraic (Estrin form of the
        Hermite polynomial and the psi update; changes bits at ~1e-16, needs a deliberate golden note). dsp-engineer.
  - [ ] (#288 review N1) tube: add a golden at transformer 100 %, sag 100 % (eco and standard) so the
        output stage's own output, including #288's one-step rail lag, is pinned; today every golden runs
        with the stage bypassed. dsp-engineer.
  - [x] (#260) `test_dsp_transformer_generic` and the shared `f64x2.h`.
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
- [x] N2 slice 4: tape (Jiles-Atherton hysteresis, head bump, gap loss,
      NAB and IEC record and playback EQ, wow and flutter) (#227). The bundles are
      `EXPERIMENTAL` (built and tested, installed only with
      `-DSONORA_INSTALL_EXPERIMENTAL=ON`) until the CPU class is held. dsp-engineer.
  - [x] (#256) Tape CPU class B (60 us p99.9) held in eco: Langevin table and a differential-algebraic
        integrator, one substep per sample in eco, `tape_run_pair`; EXPERIMENTAL lifted. Standard and high are
        reported, not held (ADR-0011 §3.5).
  - [x] (#261) Tape follow-ups from #256: c clamp, aarch64 FPCR.FZ denormals, golden against the 512-substep reference.
  - [ ] Tape follow-ups (#256 review): `SONORA_RT` on `TapeHysteresis::sync()`/`reset()`; record the standard/high CPU
        bars measured on the hot case (the eco hot case has about 10 % to spare; a quiet reference-machine run is still to record).
  - [ ] Tape follow-ups (#261 review): `cpu.p999_kernels` (1.66 x 1.15 = 1.91) is looser than the calibrated class bar (1.84), so it
        never trips: re-measure unloaded and fix the value or the note; add tape to the nightly calibrated gate if it should be held.
        `test_reversibility_limit` should assert near the measured 1.5e-4 Ms remanence (bar about 5e-4). Run `test_dsp_dynamics` and
        `test_dsp_tape` on real aarch64 hardware.
  - [ ] Move the tape pair file onto the shared `f64x2.h` (#260), and document the V interface or add a `static_assert` concept check.
  - [ ] Tape aliasing above 8 kHz is a real defect (19 kHz tone, drive 12 dB: eco -4 to
        -11 dBc, standard -23 to -37, high -52 to -62 at 44.1/48 kHz): reduce it and
        extend the aliasing bar to 19 kHz.
  - [x] (#286) `thd.nominal_pct` for tape re-measured: a measured band at drive 0 and 12 dB, held in every quality.
    - [ ] (#286 review) Write the regeneration command for the THD bands (`sonora-dsp-measure --processor tape`
          with `SONORA_DSP_QUICK=1` and the JSON rows) into `brain/research/native-tape-n2.md` "THD bars".
    - [ ] (#286) `thd.h2_below_h3_db` stays at -20 dB against a measured -102.7 dB (H2 is at the numerical
          floor, -146 dBc); tighten to about -90 dB or drop it.
    - [ ] (#286 review) Add the 44.1 and 96 kHz points to the THD band check, or say in the quality.json
          note that the bands are 48 kHz only.
  - [ ] Tape goldens under Clang and MSVC (the substep count is discontinuous);
        rt-safety-auditor run; RTSan.
  - [ ] The quick `native_tape` run took about 103 s on a loaded host (target 60 s):
        re-time it in CI; if over 60 s, a cheaper denormal check. dsp-engineer.
- [x] N2 slice 5, `feature/dsp-n2-noise-hum-recovered` (PR #141): modelled noise and hum in
      `common/dsp` (`noise.h`: seeded xoshiro256** and near-Gaussian noise,
      a hum oscillator bank; `supply.h`: a bridge rectifier, reservoir and
      RC filter integrated to steady state; `triode.h`: the stage's
      thermal and shot noise and its supply transfer, small-signal), each
      held to ngspice on the same circuit (`measure/spice_noise.h`, ctest
      `spice_noise_0..4`), wired into the tube drive as `noise`, `hum`
      (0-100 %, 100 % the modelled level, 0 % digitally silent) and
      `mains` (50/60 Hz). Off by default until the owner rules on item 7.
      Measurements: `brain/research/native-noise-hum-n2.md`. dsp-engineer.
- [ ] N2 slice 5 follow-ups: (a) flicker (1/f) noise of the triode, from
      measurements of the owner's tubes or a citable published figure (no
      12AX7 figure found that we could cite; Blencowe's measurements are
      the lead). (b) Heater hum at the mains fundamental (heater-cathode
      leakage and capacitance): needs a measured or published coupling
      figure. (c) Each instance's contribution in dBFS in its editor
      (ADR-0011 §3.3, "do not stack silently"): needs the N1 telemetry
      region (with M9); the suite reports the figures today. (d) Wire noise
      and hum into the transformer (slice 3) and tape (slice 4) processors
      when they exist; tape hiss from the tape's own published noise
      spectrum. (e) Owner item 7, the default: flip `noise` and `hum`'s
      default to 100 % if the owner rules "on". dsp-engineer.
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
- [x] (#281) Timing-ratio bars are reported, not held, under a sanitizer (the `native_fet` flake).
  - [ ] (#281 review) Re-align the continuation lines of the converted `check_time_le` calls in the fet and opto
        `suite.cpp` and `measure/checks.cpp`.
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
- [ ] M-1c follow-up: a fader ride during an unanswered Open may change the
      gain of the track at that index in A. Non-blocking; ui-engineer.
