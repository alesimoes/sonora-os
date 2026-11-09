# ADR-0058: Side-chain key source on an insert

Status: Accepted (2026-11-09, with the contract: items 1 and 5 implemented; architect 2026-11-04, recommended defaults, nothing here needs the owner).
Backlog: `ui.mixer.sidechain` (the whole item), `ui.mixer.io` (its aux/Master inserts line is NOT in this ADR).
Builds on: ADR-0012 amendment 2026-11-01 (the key is the region's last planes; `InsertChainRef::key_channels`), ADR-0022 (aux buses), ADR-0024 (bus graph, loops), ADR-0027 (lanes).

## Context

PR #224 settled the wire between the engine and the plugin host: a keyed chain's region carries signal plus key planes, `InsertChainRef` names the split, and ADR-0012's amendment already chose *where the key comes from* (item 3: an aux bus's sum before its return fader) and *how the plan carries it* (item 4: a second input edge of the InsertChain node). Three things were left as follow-ups and are what this ADR fixes: the model field and its undo, the plan edge, the supervisor spawning with `key_channels`, and the operator's chooser. Today a keyed slot is a counted shape fault (silence), so a compressor that has a key input is silent on the track.

## Decision

1. **Model.** `Insert` gains `key_bus` (an aux bus `Id`, 0 = none). It is structure, not mix state: `SetInsertKey {track, insert, key_bus}` is one undo step through History, self-inverse, refused for an unknown insert or bus, for a plugin that declares no key input (the catalogue's side-chain flag, ADR-0012 §catalog), and for a key that would form a loop (item 3). Removing the bus does **not** refuse: the operation that removes a bus also clears every `key_bus` naming it inside the same Compound member list, so undo restores both; a stale id met in an old file is treated as "keyed by silence" and shown, never refused at play (ADR-0012 amendment item 4).
2. **Plan.** `plan_builder` gives an InsertChain node whose insert is keyed a second input edge from the key bus's `AuxSum` node (its sum before the return fader and before the bus's own inserts' makeup, i.e. the node that already exists for the bus's output edge). PDC (`max_in`) aligns signal and key without new code, as ADR-0012 §7.3 says. The key's channel count is the chain's (stereo bus onto a mono chain: each side × 0.5 in the node reading it).
3. **Loops.** The key edge joins the bus graph that ADR-0024 already checks: track → send → bus → key of a chain on that same track, or on a track/bus that feeds the bus, is a loop. `check_invariants` and the operation both refuse it, naming the path as ADR-0024 does. A chain keyed by a bus that its own track's output feeds *through the Master only* is not a loop.
4. **Host spawn.** `PluginHosting` passes `key_channels = chain signal channels` (1 or 2) when any insert in the chain has a live `key_bus`, 0 otherwise. Changing the key bus between none and some restarts the chain make-before-break (ADR-0012 §restart); changing between two buses is a plan swap only (same layout). Only one insert per chain may be keyed in this ADR (the host's `Load::key_channels` is per chain).
5. **Formats.** Session format and protocol each bump by one, at the next free number when the code lands (the daemon reserved none; the contract task takes them from the then-current `kFormatVersion` and `kProtocolVersion` and says so in its PR). Window format bumps only if the undo window codec carries `SetInsertKey` (it does: one new op code, appended). Older engines answer the new command NotImplemented; an older session file is read with `key_bus` 0.
6. **UI (Mix window).** The insert slot shows a key chooser (a small "Key" popup beside the slot, enabled only when the plugin has a side-chain input) listing "None" and the session's aux buses by name; a keyed slot shows the bus name, a missing bus shows "Key: (missing)" in the warning colour. Pro Tools' "Key input" menu is the workflow reference; its bus-or-input choice is the same, hardware inputs as keys are not offered (item 6 of Alternatives). The chooser is a plain control-thread command, no audio-thread involvement.

## Implementation notes (contract, 2026-11-09)

- The model cannot see the catalogue, so item 1's "plugin that declares no key input" is refused by engine/control's catalogue check before `apply()`, from the new `PluginInfo::key_inputs` (the probe record's `sidechain_in`); the model refuses the rest (unknown track, insert or bus, a second keyed insert, a loop). The refusals start with fixed words (`insert_key_refusal` in `common/model/include/sonora/model/insert_key.h`).
- The key edge joins the bus graph in `bus_path` and `bus_loop_problem`: a bus that keys a track reaches that track's output and send buses. So a later `AddSend`, `SetOutput` or `MoveInsert` that would close a loop through a key is refused too, naming the path.
- Session format 26, protocol 69 (25 and 68 on main when the work started; the daemon reserved none), undo window format 21 (an insert carries `key_bus`). `ReplaceInsert` leaves the replacement unkeyed; undo restores the keyed one.

## Rationale

The expensive part was the contract, already paid in #224. Reusing the aux bus keeps every new rule (loops, PDC, solo, naming) inside code that ADR-0022/0024 already test; a new kind of routing object would need all of them again. Making it an Insert field (not a send) keeps `Send` meaning "audio to listen to" and lets the plugin host's layout follow the insert.

## Consequences

- One more model field, one op, one plan edge kind; format and protocol bump; FakeEngine and conformance gain the command.
- A track as a direct key source (Logic) is not done: it would be a second edge kind from a `TrackOut`, same node, no further contract change.
- A keyed chain costs one more input copy per block (a memcpy of one plane pair into the region), inside the existing per-callback budget; **assumption, unmeasured**: the plugin-engineer measures the 18-track case with every chain keyed (`SONORA_DSP_QUICK` is not enough; use the engine's stress test).
- Hardware key inputs (a mic on a spare input keying a gate) need ADR-0024 §5's I/O setup; that item stays on `ui.mixer.io`.

## Unverified

- That DPF's and third-party LV2 side-chain ports behave when the key is silent for the whole run (an unconnected bus): assumption from the host's null-pointer patch (ADR-0011); a host test with a keyed compressor and a removed bus settles it (researcher, if the test shows a click).
- The cost figure above.

## Alternatives rejected

1. **Key source as a send flag** (a send marked "key"): sends mean audio-to-hear, post/pre and mute semantics would need exceptions; the loop and solo logic would see a phantom send.
2. **Key as a track `Id` directly (Logic style) now:** the track's pre-fader node exists, but then a key depends on a track's inserts and PDC in ways ADR-0022 avoided; deferred, not refused.
3. **Per-chain rather than per-insert key field:** a chain may mix a keyed gate and an unkeyed EQ; the field belongs to the insert that has the port.
4. **Key as mix state (not undoable), like levels:** it is routing; a loop-refused, undoable edit matches `SetOutput` (ADR-0024).
5. **Silent fall-back to unkeyed when the key bus is gone:** a compressor that suddenly listens to itself changes a mix silently; silence on the key plus a visible warning fails loudly.
6. **Hardware inputs as keys now:** needs I/O setup paths that do not exist.
