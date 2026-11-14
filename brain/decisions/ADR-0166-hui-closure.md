# ADR-0166 — Closure record: drivers.surfaces.hui

Status: accepted (closure record, no new decision).

## Summary

- Item `drivers.surfaces.hui` (HUI control surface over a MIDI port, ADR-0055)
  is closed by reference to the work already on `main`.
- Landed under ADR-0055 in commit `a57ed021` (bundle #435, os-drivers):
  `drivers/surfaces/src/hui.cpp` (`HuiProtocol`), its header
  `drivers/surfaces/include/sonora/surfaces/hui.h`, and its test
  `drivers/surfaces/tests/test_hui.cpp` (CMake target `test_hui`, ctest name
  `surface_hui`).
- No code changed by this closure. No contract change. No protocol change.
  `kProtocolVersion` untouched.

## Context

- ADR-0055 describes the HUI and MCU surface protocols over `MidiPort` and
  lists the contract gaps the surface work found in `contracts/surfaces`. Those
  gaps belong to the contract's owner (ADR-0040) and are not part of this item.
- Before this record, the HUI item had no closure record: its status was only
  implied by the code on `main`. ADR-0162 closed the MCU item the same way;
  this ADR does the same for HUI, in the same form.
- `hui.h` states its own limits in its header comment: the contract has no
  time feedback (ADR-0055), and the transport and channel-select port order is
  read from the layout table, not stated in prose, so it must be checked on a
  device.

## Decision

- Mark `drivers.surfaces.hui` closed as the HUI surface over `MidiPort`
  (ping and online state, zone and port switching for the strips, faders with
  touch, V-Pot rings, LEDs for mute, solo, arm and select, scribble text, the
  8-digit time display, banking in the protocol, transport and loop buttons)
  as shipped.
- Keep the contract gaps and the engine-side strip mapper open, under their
  own items. Do not work around them inside `drivers/surfaces`.

## Rationale

- The code and its test are on `main` and build from the current tree: the
  narrow micro build and its ctest run pass (see Evidence). A closure needs
  that evidence, not a claim.
- Keeping the contract gaps out of this item means the closure does not hide
  a contract change inside a driver PR. ADR-0055 already asks the contract
  owner for those changes.

## Consequences

- The item leaves the open list. A future change to HUI behaviour is a new
  item with its own evidence.
- The contract gaps listed under "Not in this item" stay open.
- Device configuration (which MIDI node is a surface, and HUI or MCU for each)
  stays open; it needs a real device to be meaningful.

## Evidence

Confirmation that the code is on `main`:

    $ git log origin/main --oneline -- drivers/surfaces/src/hui.cpp drivers/surfaces/tests/test_hui.cpp
    a57ed021 Bundle os-drivers: #402, #407, #414, #418, #419, #429 (#435)

Narrow build through the build slot, over the two files, in the worktree on
`sub/drivers.surfaces.hui/close` (started from `feat/drivers.surfaces.hui`):

    $ tools/build-slot.sh tools/micro-build.sh --files-from <list of
        drivers/surfaces/src/hui.cpp, drivers/surfaces/tests/test_hui.cpp>
    [  0%] Built target sonora_api
    [100%] Built target sonora_surface_mcu
    [100%] Built target test_surface_bridge
    micro-build: ctest -R '^(surface_engine_bridge|surface_hui|surface_mcu|surface_mtc_contract|surface_mtc_sync|surface_raw_midi_port)$'
    1/6 Test #685: surface_mcu ......................   Passed    0.01 sec
    2/6 Test #686: surface_hui ......................   Passed    0.01 sec
    3/6 Test #687: surface_engine_bridge ............   Passed    0.00 sec
    4/6 Test #688: surface_raw_midi_port ............   Passed    0.00 sec
    5/6 Test #689: surface_mtc_contract ............   Passed    0.00 sec
    6/6 Test #690: surface_mtc_sync ................   Passed    0.00 sec
    100% tests passed, 0 tests failed out of 6
    micro-build: passed
    micro-build: rtsan skipped (nothing built)
    micro-build: ok build (45 s)
    micro-build: all passed

The output above is trimmed (the container's cache statistics and the `Start`
lines are omitted). `surface_hui` and `surface_engine_bridge` are the two
named tests; they ran inside the micro build's ctest selection, which is a
superset of the two named. No file was changed to get this result.

## Alternatives rejected

- Reopen ADR-0055 and close the item there: rejected, because ADR-0055 is an
  accepted decision with its own open amendment; a closure record is the
  narrower and clearer artefact.
- Close the item together with the contract gaps: rejected, because the gaps
  need the contract owner's amendment and a protocol decision, and bundling
  them would make this a contract change.
- Fix the gaps with a driver-side workaround: rejected, as ADR-0055 says, since
  it would hide the contract problem.

## Not in this item

- The contract gaps of ADR-0055 that the HUI work meets: no time feedback to
  the surface (the 8-digit display stays unfed by the contract), and the other
  gaps ADR-0055 lists for the shared contract.
- The engine-side strip mapper (strip to track, bank offset, touch-gated fader
  write-back).
- Device configuration (which MIDI node is a surface, and HUI or MCU for each).
- The transport and channel-select port order, which `hui.h` says is read from
  the layout table and must be checked on a device.

## Unverified

- No real unit was attached. The HUI ping reply, the zone and port layout, the
  meter rate, the touch and fader feedback and the time display are checked
  only against the published report cited in `brain/research/hui-protocol.md`
  and the fake port of `test_hui.cpp`.
- The test run covers the build on this host's build image only. It is not a
  full-tree run; the full suite is the nightly's.
- The ctest selection named in the task (`surface_hui|surface_engine_bridge`)
  was not run as a separate command; both tests ran inside the micro build's
  wider selection shown above.
