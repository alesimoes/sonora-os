---
name: faceplate-design
description: Design a plugin faceplate or editor so it looks like the original hardware without copying it or using its brand. Use when a processor gets its editor, or a screen is redesigned.
---

# Faceplate design

1. Read `.claude/rules/design-faceplates.md` and the processor's parameter
   list (`plugins/native/<name>`).
2. Describe the reference hardware from the published, general idea of the
   type (control layout, meter style, colour mood). Do not use photos or
   assets of a commercial product, and write no brand, model or artist name.
3. Give the unit our own name and a short palette (4-6 colour tokens, theme
   classic and modern).
4. Lay out controls in the signal-flow order of the hardware; group by
   function; sizes in px at 1x, minimum hit target 24 px.
5. Write the spec table into `docs/images/<name>-spec.md` and a mockup image
   beside it:
   | control | param id | range | unit | taper | px | colour token |
6. Assets: vector first; note licence of every font and image.
7. Hand the spec to `ui-engineer` (builds it) and `dsp-engineer` (confirms
   ranges and tapers match the DSP). Anything unmeasured (render cost, DPI
   scaling) is listed as unverified.
8. Screenshots of the finished editor are not committed (owner, 2026-11-13).
