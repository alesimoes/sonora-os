# HUI protocol: what we implement and where it comes from

Written for drivers.surfaces.hui (ADR-0055). No vendor code was read or
copied (ADR-0004). **The sources are community reverse-engineering, not an
official specification**: Mackie never published one (the report below says
so), and the report itself is the result of sniffing one unit.

## Sources

1. "MACKIE HUI MIDI protocol", the results of a 2-day reverse-engineering
   session by "theageman" (SSEI, 5 January 2010, firmware 1.45 of the unit
   tested). A PDF circulated on forums (the copy read was attached to
   https://forum.bome.com, `aX7AWqj6FpgMiEMQi0kACDDTSSi.pdf`; copies on
   Scribd and manualmachine.com). The only byte-level description found; every
   row below is from it unless another source is named. It is a single unit
   at a single firmware.
2. Raphael Doursenaud's note in the TouchMCU repository (MCU document,
   foreword) mentions independent HUI reverse engineering with a BabyHUI; no
   findings were published there, so it confirms nothing.
3. theOtherMichael/HUI-MCU-Logger (MIT), `InterpretHUI.cpp`: a decoder of a
   DAW's HUI output. Cross-checks the host-to-surface direction: ping is
   note 0 velocity 0; zone select `B0 0C`; port `B0 2C` with bit 6 = on;
   fader `B0 0n` / `B0 2n`; V-Pot ring `B0 1n`.
4. ChiilThenkt/ArduinoHUI (GPL-3.0), an Arduino HUI surface emulation:
   cross-checks the ping reply (`90 00 7F`), zone select `0C` / port `2C`,
   the time display as SysEx command `0x11` (digits rightmost first), and
   zone `0x16` as the timecode LEDs.

Cross-checked between 1 and 3/4: ping and reply, zone/port select, fader
pair, ring, time SysEx. Single-source (source 1 only): the surface-to-host
direction (`B0 0F` / `B0 2F`, V-Pot `B0 4n`, jog `B0 0D`), the small display
SysEx, meters, zone and port numbers.

## Encoding used

All status bytes are channel 0. "Host" is us.

| Item | Encoding |
|---|---|
| Ping | Host sends `90 00 00` about once a second (we use 1000 ms); the surface answers `90 00 7F`. Without pings the surface goes offline after about 2 s and its faders stop obeying; everything else still works. |
| Power cycle | The surface sends `FF` (system reset). We treat it as offline and redraw. |
| Switch, surface to host | `B0 0F zz` (zone) then `B0 2F yp`: y = 4 on, 0 off, p = port 0..7. Always sent as a pair, no running status. |
| LED, host to surface | `B0 0C zz` then `B0 2C yp`, same y/p. Several may follow one zone select. The controller numbers differ by direction (0F/2F in, 0C/2C out). |
| Channel strips | Zones 0..7 = strips 1..8. Ports: 0 fader touch, 1 select, 2 mute, 3 solo, 4 auto, 5 V-Sel, 6 insert, 7 rec/rdy. (Report example: solo on the fifth strip is zone 04, port 3.) We use 0, 1, 2, 3, 5, 7. |
| Fader | Move, either direction: `B0 0z hi` + `B0 2z lo`, z = strip 0..7, 14 bit. The unit's resolution is 9 bit, so it zeroes the low 5 bits of lo. Touch: zone z, port 0 (`B0 0F 0z`, `B0 2F 40`); release `B0 2F 00`. |
| V-Pot rotation | `B0 4p vv`, p 0..7 strips (up to 0xC for other pots; 0xC is "scroll"); vv > 0x40 is +(vv-0x40), vv < 0x40 is -vv. |
| Jog | `B0 0D vv`, same delta encoding, about -15..15. |
| V-Pot ring LEDs | `B0 1y vv`: 11 LEDs 0..A, centre is 5. Mode 0 dot: vv 1..0xB is LED 0..10. +0x40 lights the small LED under the pot. We send dot mode, centre lit (0x46). |
| Meters | `A0 0y sv`: y strip, s side (0 left, 1 right), v 0..0xC. The report lists dB per step (0xC at or above 0 dB, 0xB -2, 0xA -4, 9 -6, 8 -8, 7 -10, 6 -14, 5 -20, 4 -30, 3 -40, 2 -50, 1 -60). We send 0..12 from the feedback's 0..1 and put the same value on both sides. |
| 4-char display | `F0 00 00 66 05 00 10 yy c1 c2 c3 c4 F7`, yy 0..7 = strips, 8 = SELECT/ASSIGN. Characters 0x00..0x7F; ASCII 0x20..0x7E is as ASCII (report's character table). The contract carries 7 characters; we send the first four. |
| Time display | `F0 00 00 66 05 00 11 y0 .. y7 F7`, y0 rightmost. 0x00..0x0F is 0..F; add 0x10 for a decimal point (not valid on y0). No blank digit is documented: we send 0. |
| Transport (zone 0E) | Ports in the report's layout table order: 0 talkback, 1 rewind, 2 fast fwd, 3 stop, 4 play, 5 record. |
| Loop (zone 0F) | 0 RTZ, 1 end, 2 on line, 3 loop, 4 quick punch. We use 3. |
| Channel select (zone 0A) | 0 channel left, 1 bank left, 2 channel right, 3 bank right. The prose confirms only "next bank" = zone 0A port 3. |

## Verified in this change

Our encoder and decoder agree with the table: `test_hui` writes every
expected byte string out literally, and one test drives the shared
`EngineSurfaceBridge` end to end. That proves the code matches this note, not
that a unit accepts it.

## Not verified, not implemented

- Any real HUI or HUI-mode unit (also a Mackie Baby HUI). Single-source
  details above are the first to check.
- The transport and channel-select port order is read from the layout table,
  whose text extraction in the copy we have is garbled in places (row labels
  shifted); the zone numbers used agree with the zone-name list, and with
  sources 3 and 4 only for zones 0x16 and 0x1D.
- The 2x40 main display (command 0x12), the SELECT/ASSIGN display, auto
  mode, insert, V-Sel as a mode, scrub/shuttle, num pad, function keys,
  footswitches, click and beep.
- A blank digit on the time display.
- Meter step mapping: we do not map dB to the report's steps; the bridge's
  0..1 value is scaled linearly to 0..12 as for the MCU.
- Whether a surface needs a ping before the first fader command is
  documented only as "faders do not work without it"; we ping on the first
  render.

## Behaviour on the wire

Same rules as the MCU (mcu-protocol.md): faders echoed; a touched fader is
not driven; meters rate limited to 50 ms and sent on change; unchanged state
never resent; a bank change redraws from the cache. A fader whose write the
port rejects is resent by the next change.
