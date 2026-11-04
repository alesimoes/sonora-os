# Mackie Control Universal protocol: what we implement and where it comes from

Written for drivers.surfaces.mcu (ADR-0055). No vendor code was read or
copied (ADR-0004). The encodings below come from published descriptions of
the MIDI protocol, cross-checked against one another where more than one was
found; anything found in a single place is marked.

## Sources

- The MCU / Logic Control MIDI encoding reference in the TouchMCU
  repository, `doc/mackie_control_protocol.md`
  (https://github.com/NicoG60/TouchMCU). Its licence was not visible in the
  excerpt read; we took facts (note numbers, CC numbers), not text.
- The Ardour project's published Mackie surface source and commit log
  (https://git.ardour.org/ardour/livetrax, `libs/surfaces/mackie`), read for
  behaviour only: faders must be echoed back to a motorised surface, and the
  14-bit range is 0..16383.
- User-community notes listing fader pitch-bend channels E0..E7 and E8 for
  the master.

## Verified in this change (by tests against a scripted device)

Our encoder and decoder agree with the table below. That proves the code
matches this note, not that a real surface accepts it.

| Item | Encoding used |
|---|---|
| Fader position | Pitch bend, status E0+strip (0..7), 14 bit, LSB then MSB, 0..16383. Master (E8) ignored: the contract has no master strip. |
| Fader touch | Note 0x68+strip, velocity 127 touched, 0 released. |
| Rec, Solo, Mute, Select, V-Pot push | Notes 0x00, 0x08, 0x10, 0x18, 0x20 + strip. Acts on press (velocity > 0). LED: same note, 127 on, 0 off. |
| Bank left / right | Notes 0x2E / 0x2F (8 strips). Channel left / right: 0x30 / 0x31 (1 strip). |
| Transport | Rewind 0x5B, Forward 0x5C, Stop 0x5D, Play 0x5E, Record 0x5F. Lights: same notes. |
| V-Pot rotation | CC 0x10+strip, bit 6 = counter-clockwise, bits 0..5 = detents. |
| Jog wheel | CC 0x3C, same encoding. |
| V-Pot LED ring | CC 0x30+strip: bits 0..3 position, bits 4..5 mode, bit 6 centre LED. We use mode 0 (single dot), positions 1..11, centre LED lit at 6. |
| Meters | Channel pressure D0, data = strip << 4 or level; 0..0xC is 0 to 12 steps (0xC = 0 dB). We never send the overload codes (0xE set, 0xF clear). |
| Scribble strip | SysEx F0 00 00 66 14 12 <offset> <ASCII...> F7. Offset 0x00..0x37 upper line, 0x38..0x6F lower line; 7 characters per strip at offset strip*7. |
| Time display | CC 0x40..0x49, digit 1 (rightmost) at 0x40; bit 6 decimal point, bits 0..5 the character (ASCII & 0x3F). |

## Not verified, not implemented

- Cycle button note 0x56: from memory of Logic Control layouts, found in no
  source read here. It is wired to Loop and is the first thing to check on a
  real device.
- The connection handshake (host connection query 0x01, reply 0x02 with a
  challenge response). The reference documents it; we did not implement the
  response algorithm because we could not verify it without a device. We
  ignore incoming SysEx. Surfaces that wait for the reply (a real Mackie
  unit) may stay dark; Logic Control compatible ones, and most X-Touch
  modes, do not wait. Needs a device.
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
