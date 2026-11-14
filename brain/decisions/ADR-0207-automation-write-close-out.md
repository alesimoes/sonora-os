# ADR-0207 - Automation write: close-out, and the AutoMatch preference split

Status: accepted (architect, 2026-11-14)

## Context
Backlog item `automation.write` was worked in three parts. ADR-0131 (#692)
recorded what remained after the earlier slices: Latch/Write keeping the
static, the fader cap following a written release, and an AutoMatch time
preference. #692 merged the ADR, the UI glide and the Touch return edge
cases into `main`. What is left of the item is one control: the AutoMatch
time in Preferences. It needs a protocol message, and no protocol number is
reserved for it. The default of 250 ms is also still unverified: nobody has
read published Pro Tools material on how its AutoMatch return is timed.

## Decision
1. Every other part of `automation.write` is on `main`. The commits are:
   - 708ab5ca (#282): Touch's return to the static value and `--automatch-ms`
     (ADR-0027 section 8 slice 4).
   - b3f49ed2 (#290): Trim, with Touch/Trim and Write/Trim and a VolumeTrim
     lane (ADR-0027 section 8 slice 5).
   - 7c794eef (#439): Latch/Write keep the static while rolling
     (ADR-0131 section 1; no code was needed in #692).
   - 6fd7e9ab (#692): ADR-0131, the fader cap's Touch release glide, pan
     kept static in a written ride, and the Touch return edge cases (a new
     plan ends the return, a ride across a stop, a released Touch on a track
     with no region in the plan).
2. The AutoMatch time in Preferences is split out of `automation.write` into
   its own backlog item. That item waits for a protocol number reserved by
   the coordinator. Until then the engine flag `--automatch-ms` stays the
   only control, as ADR-0131 section 3 decided. The coordinator files the
   BACKLOG line; this ADR does not edit `planning/BACKLOG.md`.
3. The 250 ms default stays unverified. It is not changed until a researcher
   item reads published Pro Tools AutoMatch material (ADR-0131 section 5).
4. `automation.write` is closed as delivered, minus the split item in
   decision 2.

## Rationale
The remaining work is one setting. A setting that the engine cannot read is
a dead control, and the rule is to refuse rather than show a control that
lies. So the setting waits for its protocol message, and the item closes
without it. Keeping the item open for one control would hold back the rest,
which is already on `main` and tested.

Keeping the 250 ms default is the cautious choice. It is a guess, and the
engine and the cap both use it, so changing it without a source would make
the mismatch worse, not better.

## Consequences
- No protocol, format or `contracts/` change. This ADR and its item change
  no code and no wire format.
- `automation.write` can be marked done on the BACKLOG once the coordinator
  adds the split item.
- The Preferences AutoMatch control cannot be used until its item gets a
  protocol number. Until then `--automatch-ms` is the only control.
- The cap and the audio can still differ briefly when the flag is not 250 ms
  (ADR-0131 consequences).
- The 250 ms default remains a guess until the researcher item answers.

## Alternatives rejected
- Keep `automation.write` open until the Preferences control lands: it holds
  back finished work, and the control has no protocol number to wait on.
- Reserve a protocol number here: numbers are reserved by the coordinator at
  launch, not by an ADR.
- Change the default to a figure from memory of another DAW: unverified, and
  the source is proprietary, so it cannot be read from the product itself.
- Add a Preferences control now that writes a local value only: a dead
  setting, the same objection as ADR-0131.
