# ADR-0194 - Close out beat detection: ADR-0114 stands, unmeasured constants are listed

- Status: Accepted
- Date: 2026-11-14
- Item: `time.beat_detection`
- Relates to: ADR-0114 (the decision this closes), ADR-0011 §4.6 (provenance
  of any labelled recordings)

## Summary

`time.beat_detection` is closed as a feature. ADR-0114 remains the decision:
an offline `sonora-analyze beat` pass, a tempo map from `tempo_from_beats`,
applied through ADR-0050's tempo operations and undone with Undo Tempo. Its
contract, detector, tempo-map model (#603) and the `tempo.edit detect` UI
(#711) are on main. This ADR adds no code and changes no interface.

The figures that are not measured are listed in
`brain/research/beat-detection-accuracy-todo.md`: beat accuracy on real drums
and vocals, the 0.4 confidence constant, the 0.3 apply threshold, the 4 %
stability bound and half/double-tempo errors. Each has the measurement that
settles it. Until those are run, no document may describe the detector as
accurate on real material.

## Decision

1. The item is closed. Remaining accuracy work is tracked as the research
   note's measurements, not as open code tasks.
2. The constants in ADR-0114 and the code keep their present values until a
   measurement in the note changes them. A changed value is a new dated
   amendment with the measurement report attached.
3. The labelled recordings behind those measurements follow ADR-0011 §4.6:
   our own or licensed and recorded on a card, with provenance cleared before
   use.

## Consequences

- The feature's status is closed on main; the BACKLOG line stays ticked only
  when the coordinator's chore PR updates it.
- No code, interface or protocol change. `brain/INDEX.md` gets its line from
  the coordinator after the merge, not from this ADR.

## Alternatives rejected

- Keep the item open until the measurements run. Rejected: it blocks the
  feature's closure on hardware-free material the owner has not yet provided,
  and the constants are already guarded by the synthetic tests and the 0.3
  refusal.
- Tune the constants now from the synthetic clicks. Rejected: a fit to
  synthetic material would look measured without being so.
