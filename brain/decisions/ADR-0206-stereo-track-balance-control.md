# ADR-0206: Stereo track balance control in the channel strip

Status: accepted (2026-11-14)

## Context
`TrackMix::pan` is one value in -1..1. The engine's `pan_gains` already treats it as a balance for a track of two or more channels, but the strip drew the same bar for every track, so a stereo track's control looked like a mono pan.

## Decision
- The strip reads the track's channel count (`SessionModel::trackChannels`, read-only; `MixWindow` binds it to `ChannelStrip.channels`). Two channels or more: the same bar gains a centre detent and L / R end marks; mono is unchanged.
- No new value, no new command, no protocol: drag (Ctrl fine), Alt-click centre and the typed field (L30, R30, C) all end in `TrackMix::pan` through the existing SetMix path.
- The control stays inline in `ChannelStrip.qml`; no new QML file.

## Rationale
One value keeps session files, undo and automation as they are; only the label of what the engine already does changes. Inline avoids a build-list edit for a few dozen lines.

## Consequences
A track's channel count changed after load needs a re-read of `trackChannels` (the strip binds on the track index). Not checked on a real UI session beyond the tests.

## Alternatives rejected
Separate left and right pans (dual-mono pan; protocol and file change, out of scope). Hiding the control on stereo (loses a control the engine honours).
