# ADR-0025 — Launch scope: video and immersive audio come after launch

- Status: **Accepted**, 2026-10-31, by the owner.
- Date: 2026-10-31
- Amends: planning/ROADMAP.md, DAW 10 and DAW 11.
- Numbering: ADR-0024 is reserved by the open mixer I/O work (`t4-mixer-io`).

## Summary

*Navigation only; the sections below are the decision.*

- §1: video work and surround/immersive audio are optional future features,
  not launch requirements.
- §2: control surfaces and sync stay in the launch scope as DAW 10.
- §3: the progress catalogue keeps the deferred items in their own phase,
  left out of the launch percentages.

## §1 What moves after launch

The owner's words (2026-10-31): editing video and immersive sound in our
DAW "serão feitos como opcional no futuro. não é um requisito para o
lançamento" (will be done as optional features in the future; not a launch
requirement).

| Catalogue item | Was |
|---|---|
| `video.timecode`, video frame rates and session start | DAW 10 |
| `video.track`, a video track locked to the timeline | DAW 10 |
| `surround.bus`, multichannel buses with 5.1 and 7.1 panning | DAW 11 |
| `surround.binaural`, binaural panning | DAW 11 |
| `surround.immersive`, object mixing and ADM BWF export | DAW 11 |

No agent picks these before launch.

## §2 What stays

DAW 10 keeps:
- HUI and Mackie Control;
- MTC, LTC, MIDI clock and MMC;
- word clock and ADAT;
- the direct ALSA backend if it is needed;
- key map coverage.

Timecode as an audio ruler and LTC/MTC chase are already sync work and stay
in scope. Only the video frame rates and the video track itself move.

## §3 Catalogue

`planning/progress.json` gives phase 11 the name "After launch (optional,
ADR-0025)" and lists it in `post_launch_phases`. `tools/progress.py` still
shows that phase's own row, but leaves its items out of the module, front
and overall percentages, so those measure the launch.

## Consequences

- Stereo-first design is acceptable. Where a channel count is already generic
  (for example the ADR-0022 aux buses), keep it generic, so the later
  surround work is additive.
- Rejected alternative: deleting the items. The work is still wanted, just
  later.
