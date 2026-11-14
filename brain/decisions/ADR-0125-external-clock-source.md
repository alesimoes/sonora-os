# ADR-0125: External clock source (word clock, ADAT)

Status: accepted (architect, 2026-11-09)

## Summary
A new module `drivers/clock` models which clock the interface runs from (internal, ADAT, S/PDIF, word clock), asks the card to switch, and reports lock. It is a control-thread service behind one interface, `ClockSourceControl`, with an ALSA-mixer implementation and a fake. A session that asks for an external source and does not get a valid lock **refuses to start recording** and says why; it never falls back to internal silently. Lock loss during a take is counted and shown, never acted on from the audio thread. No protocol, `contracts/` or session-format change. Whether the UMC1820 exposes any of this to Linux is unverified and handed to the researcher.

## Context
With several digital devices, all must share one sample clock or the takes drift and click. The sample clock is hardware: software cannot generate it, only select the source on the card and observe lock. Backlog item `drivers.sync.clock` ends "validated on the UMC1820". The UMC1820 is class-compliant USB with ADAT and S/PDIF; we have not confirmed it has a word-clock input or that its source selector is reachable from ALSA (the vendor panel is Windows/macOS). Linux `snd-usb-audio` exposes UAC2 clock units as mixer controls ("Clock Source N Validity", "Clock Selector") only when the device descriptors say so.

## Decision
1. Module `drivers/clock`, library `sonora_clock`, tests labelled `drivers`. Hooked from `drivers/alsa/CMakeLists.txt` (`add_subdirectory(../clock clock)`); depends only on `common/base`.
2. Types: `ClockSource {Internal, Adat, Spdif, WordClock}`; `ClockLock {Locked, Unlocked, Unknown}` (Unknown = the card does not report validity); `ClockCaps` (sources offered, whether lock is reported).
3. Interface `ClockSourceControl`: `caps()`, `select(ClockSource) -> Result`, `selected()`, `lock()`. Pure virtual, injected. `AlsaClockControl` implements it over a small `ClockMixer` seam (enumerate elements, read/write enum and bool) so tests use `FakeClockMixer`; element-name matching is a table, not scattered strings.
4. `ClockGuard` (control thread): `prepare(requested)` at session open. Requested source not in `caps()`, select fails, or lock still `Unlocked` after a bounded number of polls (a count, never wall clock) gives an error with the reason; the UI keeps record disabled. `Unknown` lock with an external source is allowed only after the user confirmed it once per device (machine settings, not the session). `poll()` runs on the existing control tick, counts lock losses and keeps the last state; it never touches the audio thread and never stops transport (stopping would lose the take).
5. The chosen source is a per-machine setting (it describes wiring), not session content: no format change.
6. Sample rate stays owned by the backend.
7. Out of scope: settings UI, the real libasound mixer adapter, mid-take recovery, hardware validation.

## Rationale
Clock is the one thing software cannot fix after the fact, so failure must be loud and early (start-up), as in "fail loudly rather than degrade silently while recording". The mixer seam isolates the unknown part (what the UMC1820 exposes): if the card exposes nothing, only the adapter changes. Not stopping on lock loss follows "a take is never lost"; the counter lets the UI warn.

## Consequences
On a card with no readable lock the feature reduces to "Unknown, confirm once". UI, the real mixer adapter and xrun correlation are later work. A card whose selector is vendor-only is unsupported until researched.

## Unverified (for the researcher)
- Does the UMC1820 have a word-clock input, and does it expose a clock selector and validity through ALSA (`amixer -c N contents`, `lsusb -v` clock descriptors) on the target kernel?
- What happens on losing the external clock (xrun, silence, glitches) and how fast does validity flip?
- Can the source change while the stream is open, or does it need a reopen?

## Alternatives rejected
- Silent fallback to internal when external fails: recordings drift against other devices with no sign.
- Stop transport on lock loss: loses the take; counting and warning is safer.
- Store the source in the session: it is wiring, differs per machine, and would need a format bump.
- Software resampling to an external clock: hides the problem, adds latency, is not sync.
- Put it inside the `drivers/alsa` backend: grows a file for an optional, hardware-dependent service and shares files with the backend squad.
