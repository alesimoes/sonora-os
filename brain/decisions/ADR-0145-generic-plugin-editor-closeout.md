# ADR-0145 - Generic plugin editor close-out

- Status: Accepted
- Date: 2026-11-12
- Item: `native.editors` (generic plugin editor), close-out of its UI follow-ups
- Relates to: ADR-0057 (plugin telemetry and descriptor hints: the telemetry
  region, `InsertModel` `panelMeterValues` / `panelCurveData`), ADR-0011
  (plugin faceplates, own names), ADR-0004 (workflow, not binaries)
- Numbering: 145 is reserved by the coordinator for this record (next free
  after ADR-0139)

## Summary

- The generic plugin editor's telemetry region and meters already shipped
  under ADR-0057 (`InsertModel` `panelMeterValues` and `panelCurveData`). They
  are not reopened here.
- `native.editors` closes on its two remaining UI follow-ups. Both are small,
  and neither changes a panel's wiring.
- Lane awareness of plugin knobs is NOT part of this close-out. It needs
  `insert_model.h`, `insert_controller.h` and `InsertPanel.qml`, which are held
  by `ui.mixer.sidechain`. It moves to a follow-up feature once that work
  merges.
- The generic panel keeps a keyboard-operable `Knob` (arrow keys; Shift for
  coarse steps; Ctrl for fine; Home resets to the default). It emits the same
  begin / moved / end / reset signals as before, so no panel wiring changes.
- The knob's accessible role is `Slider`, with the value as its text.

## Context

ADR-0057 gave the generic editor its meters and response curves. What was
left open under `native.editors` was the operator-facing control surface of
the generic panel. The knob was mouse-only: no focus, no keys, and no role
that a screen reader or keyboard user could work with. A plugin's parameter
is only usable from the keyboard if the knob that drives it is.

Two follow-ups remain:

1. Keyboard control and focus for the generic `Knob`, with an accessible
   `Slider` role and value text.
2. Lane awareness of plugin knobs, so a knob knows which lane of the insert
   it belongs to.

Follow-up 2 touches files that `ui.mixer.sidechain` holds. Taking it here
would make this PR collide with that feature, so it is deferred.

## Decision

1. `native.editors` is closed by this record. Its meters and curves are
   delivered by ADR-0057; its keyboard and accessibility knob is delivered
   by the generic `Knob` change on `sub/native.editors/knob`.
2. The generic `Knob` supports the keyboard as follows:
   - Arrow keys step the value.
   - Shift makes the step coarse; Ctrl makes it fine.
   - Home resets the value to its default.
   - Each press emits exactly one begin / moved / end sequence, and the same
     reset signal as the mouse path.
   - At either end of the range the key is a no-op and emits nothing.
3. The knob's accessibility role is `Slider`. Its accessible value text is the
   current value as shown to the operator.
4. Lane awareness of plugin knobs is deferred to a follow-up feature. It is
   scheduled after `ui.mixer.sidechain` merges and does not block this
   close-out.
5. `brain/INDEX.md` is not edited here. It is shared with active features,
   and the coordinator adds this record's line in its chore PR.

## Rationale

- The meters and curves already work; repeating them in this record would
  describe shipped behaviour twice and invite drift.
- A generic `Knob` with keyboard support gives every plugin the same control
  surface. The panel does not have to know about it, so no plugin needs its
  own QML, and the operator learns one set of keys.
- Keeping the same begin / moved / end / reset signals means the mouse path,
  the automation path and the panel wiring stay unchanged. The keyboard is an
  addition, not a second model.
- Deferring lane awareness keeps this PR off files another feature holds. The
  cost is that knobs do not yet report their lane; that gap is named and
  scheduled rather than hidden.

## Consequences

- `native.editors` is closed. Its remaining work is the lane-awareness
  follow-up, which is a new feature and not a reopening of this one.
- Operators can drive any generic plugin knob from the keyboard. Screen
  readers announce the knob as a slider with its current value.
- The screenshot for the knob's focus state (`docs/images`) is produced by the
  scene pipeline, not by this record. The PR that carries the knob must add
  it, and it is listed as unverified until it does.
- No engine, contract or protocol change is made by this record. No protocol
  version is bumped.

## Alternatives rejected

- **Per-plugin QML.** Each plugin would carry its own knob and its own keys.
  That multiplies the surface to test and document, and it breaks the single
  set of keys the operator learns. Rejected.
- **A new keyboard command layer in the model.** Routing key handling through
  the model would add a second control path beside the signals the panel
  already uses. It would also put UI concerns into `InsertModel`, which must
  stay free of them. Rejected.
- **Lane awareness in this PR.** It would touch `insert_model.h`,
  `insert_controller.h` and `InsertPanel.qml`, all held by
  `ui.mixer.sidechain`. Two features would collide on the same files.
  Rejected for this PR; deferred to a follow-up feature.
