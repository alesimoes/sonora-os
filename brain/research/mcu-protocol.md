# Mackie Control Universal protocol: what we implement and where it comes from

Written for drivers.surfaces.mcu (ADR-0055). No vendor code was read or
copied (ADR-0004). The encodings below come from published descriptions of
the MIDI protocol, cross-checked against one another where more than one was
found; anything found in a single place is marked.

## Sources

None of these is an official specification. Mackie's own MCU document is not
public; the sources are community write-ups and an Apple user manual
appendix (the Logic Control MIDI implementation), and they can contain
errors. One typo was found while cross-checking (source 1 lists V-Pot 6
rotation as hex 0x18 for CC 21, which is 0x15; and Solo 8 as decimal 8 beside
hex 0x0F). Where both sources cover an item they agree. A real unit has not
confirmed any of it.

1. TouchMCU, `doc/mackie_control_protocol.md`
   (https://github.com/NicoG60/TouchMCU), a community reverse-engineering
   write-up with a test-status column (some rows marked KO). Licence not
   visible in the excerpt read; we took facts (note and CC numbers, SysEx
   layouts), not text. Covers: notes, pitch bend, CCs, LCD SysEx, meters,
   the handshake commands and the Cycle note.
2. Apple, Logic 7 User Manual, "Host connection and initialization" (Logic
   Control MIDI implementation appendix; a copy at
   https://www.manualsdir.com/manuals/547263/apple-logic-7.html?page=240).
   The closest thing to a first-party description. Covers: Device Query,
   Host Connection Query / Reply / Confirmation / Error, the 300 ms reply
   window, serial (7 ASCII bytes) and challenge/response (4 bytes) sizes and
   the response formula.
3. The Ardour project's Mackie surface source (`libs/surfaces/mackie`), read
   for behaviour only: faders are echoed back to a motorised surface, the
   14-bit range is 0..16383, and the response formula is credited there to
   the Logic documentation (source 2). The file itself could not be fetched
   in this session, so source 3 backs behaviour, not byte values.
4. User-community notes listing fader pitch-bend channels E0..E7 and E8 for
   the master.

## Verified in this change (by tests against a scripted device)

Our encoder and decoder agree with the table below. That proves the code
matches this note, not that a real surface accepts it. Every row was
re-checked against source 1's tables on 2026-11-04 (all match); the
Handshake section below comes from sources 1 and 2.

| Item | Encoding used |
|---|---|
| Fader position | Pitch bend, status E0+strip (0..7), 14 bit, LSB then MSB, 0..16383. Master (E8) ignored: the contract has no master strip. |
| Fader touch | Note 0x68+strip, velocity 127 touched, 0 released. |
| Rec, Solo, Mute, Select, V-Pot push | Notes 0x00, 0x08, 0x10, 0x18, 0x20 + strip. Acts on press (velocity > 0). LED: same note, 127 on, 0 off. |
| Bank left / right | Notes 0x2E / 0x2F (8 strips). Channel left / right: 0x30 / 0x31 (1 strip). |
| Transport | Rewind 0x5B, Forward 0x5C, Stop 0x5D, Play 0x5E, Record 0x5F. Lights: same notes. |
| Cycle (Loop) | Note 0x56 (D6), source 1 only. |
| V-Pot rotation | CC 0x10+strip, bit 6 = counter-clockwise, bits 0..5 = detents. |
| Jog wheel | CC 0x3C, same encoding. |
| V-Pot LED ring | CC 0x30+strip: bits 0..3 position, bits 4..5 mode, bit 6 centre LED. We use mode 0 (single dot), positions 1..11, centre LED lit at 6. |
| Meters | Channel pressure D0, data = strip << 4 or level; 0..0xC is 0 to 12 steps (0xC = 0 dB). We never send the overload codes (0xE set, 0xF clear). |
| Scribble strip | SysEx F0 00 00 66 14 12 <offset> <ASCII...> F7. Offset 0x00..0x37 upper line, 0x38..0x6F lower line; 7 characters per strip at offset strip*7. |
| Time display | CC 0x40..0x49, digit 1 (rightmost) at 0x40; bit 6 decimal point, bits 0..5 the character (ASCII & 0x3F). |

## Connection handshake

Implemented from sources 1 and 2, tested against a scripted device
(`test_mcu`: `connection_handshake`, `connection_error_and_malformed_frames`,
`challenge_response_matches_published_vectors`). Header `F0 00 00 66 14`.

| Direction | Message | Layout |
|---|---|---|
| host to unit | Device Query | `<hdr> 00 F7` (`query_device()`) |
| unit to host | Host Connection Query | `<hdr> 01 ss*7 cc*4 F7`, sent at power-on and in answer to the query |
| host to unit | Host Connection Reply | `<hdr> 02 ss*7 rr*4 F7`, within 300 ms, same serial |
| unit to host | Confirmation / Error | `<hdr> 03 ss*7 F7` / `<hdr> 04 ss*7 F7` |

Response, challenge bytes c0..c3 (sources 1 and 2 print the same formula):

    r0 = 0x7F & (c0 + (c1 ^ 0x0A) - c3)
    r1 = 0x7F & ((c2 >> 4) ^ (c0 + c3))
    r2 = 0x7F & (c3 - (c2 << 2) ^ (c0 | c1))
    r3 = 0x7F & (c1 - c2 + (0xF0 ^ (c3 << 4)))

The test pins `01 02 03 04` to `05 05 7B 2F` (worked by hand) and
`12 34 56 78` to `58 0F 16 4E` (computed from the same formula, so it guards
the code, not the formula). Source 1 marks the Error command as untested on
hardware (the unit shuts down), and "Go Offline" (0x0F) as "LC only?"; we
send neither. Rendering never waits for the handshake, so units that do not
ask still work; `online()` says whether a confirmation was seen, and the
confirmation redraws the page.

Not verified: that a real MCU or X-Touch accepts our reply. Source 1 does
not say whether a unit refuses other traffic before the handshake completes.

## Not verified, not implemented

- Cycle button note 0x56 (D6, 86): now from source 1's table (Markers 0x54,
  Nudge 0x55, Cycle 0x56, Drop 0x57, Replace 0x58, Click 0x59, Solo 0x5A).
  The earlier text called it "from memory"; it is source 1's value, wired to
  Loop. Source 2 was not checked for it. Unconfirmed on a device.
- Overload LEDs, the lower scribble line, the assignment display, the
  master fader, flip, automation and function buttons.
- Meter scaling (-48 dBFS empty, 0 dBFS full) and the 20 Hz meter interval
  are our choices. The reference gives about 300 ms decay per division on
  the device; we have not measured a real one.
- Ring capacity 256 (ADR-0040) against a real surface: unmeasured.

## Behaviour on the wire

- Faders are echoed. A touched fader is never driven by the host; at release
  the motor takes the engine's value.
- Meters are sent only when the quantised level changes and at most once per
  interval, latest value wins.
- Unchanged state is never resent; a bank change redraws the page from the
  protocol's cache with no traffic to the engine.
