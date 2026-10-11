# ADR-0004 — Clone the workflow, not the binaries

- Status: Accepted
- Date: 2026-10-11

## Context

The project goal is stated as reverse-engineering Pro Tools and Logic Pro.
Taken literally that means disassembling signed, proprietary binaries for two
platforms we do not target, and reusing what we find. That is both legally
indefensible and, in practice, a dead end: the parts worth having are the
workflow and the formats, neither of which lives in the machine code.

## Decision

In scope:

- **Workflow and ergonomics.** Key bindings, edit modes (Shuffle, Slip, Spot,
  Grid), the Smart Tool, playlists and comping, mixer topology, window layout.
  How a program behaves is not protected expression; we reproduce it freely.
- **Published formats.** BWF, AAF (`libaaf`), Standard MIDI, iXML, RF64.
- **Published reverse-engineering results.** `libptformat` already decodes
  `.ptx` session files and ships under the GPL inside Ardour. Reusing published
  work done for interoperability is legitimate and we do it.
- **Documented control protocols.** HUI and Mackie Control, as documented by
  existing free implementations, so real control surfaces work.

Out of scope, and refused if asked:

- Disassembling or decompiling Pro Tools, Logic Pro or any proprietary plugin.
- Copying DSP algorithms, presets, impulse responses, samples or UI artwork.
- Circumventing iLok, licence checks or any copy protection.
- Shipping third-party trademarks, or describing our image as compatible with a
  product in a way that implies endorsement.

## Consequences

`.logicx` has no published specification and is an opaque bundle. We will not
claim Logic project import. Interchange with Logic goes through AAF or stems,
and the documentation says exactly that rather than implying more.

This ADR is binding on agents. A task that crosses the line gets refused with a
pointer to this file.
