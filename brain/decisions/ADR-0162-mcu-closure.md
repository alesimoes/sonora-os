# ADR-0162 — Closure record: drivers.surfaces.mcu

Status: accepted (closure record, no new decision).

## Summary

- Item `drivers.surfaces.mcu` (Mackie Control Universal surface over a MIDI
  port, ADR-0055) is closed by reference to the work already on `main`.
- Landed under ADR-0055 in commit `a57ed021` (bundle #435, os-drivers):
  `drivers/surfaces/src/mcu.cpp` (`McuProtocol`), its test
  `drivers/surfaces/tests/test_mcu.cpp`, the banking in the protocol,
  `EngineSurfaceBridge` (`engine_bridge.cpp`) and `RawMidiPort`
  (`raw_midi_port.cpp`), each with its own test.
- No code changed by this closure. No contract change. No protocol change.
  `kProtocolVersion` untouched.

## Context

- ADR-0055 describes the MCU protocol and lists five contract gaps the surface
  work found in `contracts/surfaces`. Those gaps are left to the contract's
  owner (ADR-0040) and are not part of this item.
- Before this record, the MCU item had no closure record: its status was only
  implied by the code on `main`. This ADR states what is on `main`, what the
  build and tests show, and what remains open.

## Decision

- Mark `drivers.surfaces.mcu` closed as the MCU surface over `MidiPort`
  (handshake, faders with touch, V-Pots, LEDs, scribble text, time display,
  banking in the protocol, transport buttons) as shipped.
- Keep the six contract gaps of ADR-0055 open, under their own items. Do not
  work around them inside `drivers/surfaces`.

## Rationale

- The code and its test are on `main` and build from the current tree: the
  narrow micro build and its ctest run pass (see Evidence). A closure needs
  that evidence, not a claim.
- Keeping the contract gaps out of this item means the closure does not hide
  a contract change inside a driver PR. ADR-0055 already asks the contract
  owner for those changes.

## Consequences

- The item leaves the open list. A future change to MCU behaviour is a new
  item with its own evidence.
- The six ADR-0055 gaps (time feedback, pan snapshot, jog scrub, master strip,
  shared fader taper, short name) and the engine-side strip mapper stay open.
- Device configuration (which MIDI node is a surface, and MCU or HUI for
  each) is still open; it needs a real device to be meaningful.

## Evidence

Confirmation that the code is on `main`:

    $ git log origin/main --oneline -S McuProtocol
    a57ed021 Bundle os-drivers: #402, #407, #414, #418, #419, #429 (#435)

Narrow build through the build slot, over the two files, in a worktree on
`sub/drivers.surfaces.mcu/close` (based on `feat/drivers.surfaces.mcu`):

    $ tools/build-slot.sh tools/micro-build.sh --files-from <list of
        drivers/surfaces/src/mcu.cpp, drivers/surfaces/tests/test_mcu.cpp>
    [  0%] Built target sonora_api
    [100%] Built target sonora_surface_mcu
    [100%] Built target test_surface_bridge
    micro-build: ctest -R '^(surface_engine_bridge|surface_hui|surface_mcu|surface_mtc_contract|surface_mtc_sync|surface_raw_midi_port)$'
    1/6 Test #685: surface_mcu ......................   Passed    0.01 sec
    2/6 Test #686: surface_hui ......................   Passed    0.01 sec
    3/6 Test #687: surface_engine_bridge ............   Passed    0.00 sec
    4/6 Test #688: surface_raw_midi_port ............   Passed    0.00 sec
    5/6 Test #689: surface_mtc_contract ............   Passed    0.00 sec
    6/6 Test #690: surface_mtc_sync .................   Passed    0.00 sec
    100% tests passed, 0 tests failed out of 6
    micro-build: ok build (130 s)
    micro-build: rtsan skipped (nothing built)
    micro-build: all passed

The output above is trimmed (the container's cache statistics and the
`Start` lines are omitted). `surface_mcu` and `surface_engine_bridge` are the
two named tests; they ran
inside the micro build's ctest selection (the micro build runs the tests that
its files reach, which is a superset of the two named). No file was changed to
get this result.

## Alternatives rejected

- Reopen ADR-0055 and close the item there: rejected, because ADR-0055 is an
  accepted decision with its own open amendment; a closure record is the
  narrower and clearer artefact.
- Close the item together with the six contract gaps: rejected, because the
  gaps need the contract owner's amendment and a protocol decision, and
  bundling them would make this a contract change.
- Fix the gaps with a driver-side workaround: rejected, as ADR-0055 says, since
  it would hide the contract problem.

## Not in this item

- The six contract gaps listed in ADR-0055 (time feedback, pan snapshot, jog
  scrub, master strip, shared fader taper, short name).
- The engine-side strip mapper (strip to track, bank offset, touch-gated fader
  write-back).
- Device configuration (which MIDI node is a surface, and MCU or HUI for each).

## Unverified

- No real unit was attached. The MCU handshake reply, the cycle behaviour on a
  device, meter scaling and ring capacity are checked only against the
  community write-ups and the fake port of `test_mcu.cpp`. See the Unverified
  section of ADR-0055 and `brain/research/mcu-protocol.md`.
- The test run covers the build on this host's build image only. It is not a
  full-tree run; the full suite is the nightly's.
