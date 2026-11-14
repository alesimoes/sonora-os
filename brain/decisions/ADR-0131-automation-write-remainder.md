# ADR-0131 - Automation write: what remains, with no wire change

Status: accepted (architect, 2026-11-09)

## Context
Backlog item `automation.write` lists Latch/Write keeping the static, the
fader cap following a written release, and an AutoMatch time preference.
The first is already on main: 7c794eef (#439) made
`PlaybackController::apply_mix` move only the static of a Latch/Write track
while rolling, and `MixModel::writes()` includes Latch and Write. That
BACKLOG line is stale. No protocol number is reserved for this slice.

## Decision
1. Latch/Write keeping the static is delivered (7c794eef, #439); build nothing.
2. The fader cap follows a written release by a local UI glide over the
   default AutoMatch time (250 ms). The state block gets no new post-fader gain.
   If `--automatch-ms` differs from 250, the cap's glide differs from the
   audio's: a cosmetic mismatch, documented here.
3. The AutoMatch time in Preferences needs a protocol message. It is deferred
   to its own feature with its own reserved protocol number; the engine flag
   `--automatch-ms` stays the only control.
4. Edge cases: a plan swap in the middle of a return cancels the return and
   snaps over one callback; a released Touch on a track with no region in
   the plan also glides; a ride released after a stop is saved as the static.
5. Unverified: how Pro Tools times its AutoMatch return. A researcher item
   must check it from published material before the default is changed.

## Rationale
The cap is a display of a value the engine already smooths; a local glide
costs no wire change and no audio-thread work. Changing the state block
would force a protocol bump and add traffic on every block for a cosmetic gain.

## Consequences
- No protocol, format or `contracts/` change in this ADR.
- The cap and the audio can differ briefly when the flag is not 250 ms.
- The Preferences control waits on a protocol number from the coordinator.
- The 250 ms default is a guess until the researcher answers (item 5).

## Alternatives rejected
- A post-fader gain in the state block: protocol bump and per-block traffic
  for a cosmetic match.
- A Preferences control now with no protocol message: it could not reach the
  engine, so it would be a dead setting.
- Re-implementing Latch/Write static handling: already on main.
