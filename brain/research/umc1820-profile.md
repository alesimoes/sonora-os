# UMC1820 device profile: what is published, what is configured, what is open

Behringer UMC1820, USB ID 1397:0503, USB audio class compliant. This note
backs `audio-stack/wireplumber-sonora.conf` section 3 and
`contracts/backend/include/sonora/device_profile.h`. Nothing here was measured
on a unit: every fact is tagged with where it comes from, read on 2026-11-04.

## Published facts

**USB ID and driver** [forum, 2016]. `alsa-info` posted by mhartzel shows
`USB Mixer: usb_id=0x13970503`; the card binds to `snd-usb-audio`, no vendor
driver.
<https://discourse.ardour.org/t/89949> (post of 2016-10-22).

**Channel count depends on the ADAT input** [forum, 2016]. The same post's
`/proc/asound/card*/stream0`: with an ADA8200 on the ADAT input at power-up,
capture is 18 channels (S32_LE) and playback 20 (S24_3LE); with nothing on
ADAT, capture 10 and playback 12. Altset 1, rates 44100, 48000, 88200, 96000.
So "18 in / 20 out" is the best case. The engine must not assume it
(`--tracks auto` exists for this). Two consequences for us: the count can
change between boots with the cabling, and sync matters (below).

**Channel order** [forum quoting the manual; second-hand]. Same post: inputs
1-8 mic/line, 9-10 S/PDIF, 11-18 ADAT 1-8; outputs 1-10 analog, 11-12
S/PDIF, 13-20 ADAT 1-8. This is **not** "ADAT 9-16, S/PDIF 17-18": the first
wording we had was wrong and the sources disagree with it. The "10 in / 12
out without ADAT" count in the previous paragraph is consistent with the
manual's order (8 + 2 S/PDIF = 10, 10 + 2 = 12). Windows pair names
("11+12 over SPDIF") quoted at
<https://www.kvraudio.com/forum/viewtopic.php?p=7921230> agree for outputs.
The Behringer quick start guide could not be fetched (403), so the table has
not been read from the manufacturer. `device_profile.h` encodes this order.

**ADAT halves at 96 kHz** [forum, 2019]. 8 ADAT channels at 44.1/48 kHz, 4 at
88.2/96 kHz (the ADAT S/MUX rule).
<https://discourse.ardour.org/t/behringer-umc1820-adat-io/101471>. The
session rate is pinned to 48000 (`pipewire-sonora.conf`), so 8 applies.
`AudioBackend::physical_capture_ports()` already says the count can depend on
rate.

**Mixer controls** [forum, 2016]. ALSA exposes "UMC1820 Output Playback
Volume", "Mic Capture Volume" and a "UMC1820 Clock Selector" with Internal,
Coaxial In S/PDIF, Optical In ADAT (the last only listed when ADAT is
present). Set from `alsamixer` (F3 playback). There is no vendor control
panel on Linux.

**Clocking** [forum, 2016 and 2019]. With an ADAT device attached the
interface must be clock-synced to it (or the other way round). An unsynced
ADAT device passes no signal (Paul Davis, Ardour forum, 2018-11-11), or
records clicks without warning. Internal clock is the default we keep.

**Mode switch reboots the box** [retailer guide, via search]. Changing the
front-panel OPT I/O between S/PDIF and ADAT while a DAW is running makes the
interface re-enumerate briefly.

**Kernel quirks** [kernel source, master, read 2026-11-04]:
- `sound/usb/quirks-table.h`: no entry with vendor 0x1397 at all.
- `sound/usb/quirks.c`, `DEVICE_FLG` table: 0x1397 entries exist for 0x0507
  (UMC202HD), 0x0508 (UMC204HD), 0x0509 (UMC404HD), 0x050c (Flow 8), 0x0510
  (UV1), setting implicit-feedback and playback-first flags. **No 0x0503.**
- `snd_usb_apply_boot_quirk_once`: every vendor 0x1397 device gets "an
  explicit device descriptor read at boot", except 0x0508 and 0x0509, which
  skip it. So the UMC1820 takes that read. This is the only Behringer-wide
  behaviour that applies to it, and it is in the kernel already.
Conclusion: no module option and no udev rule is needed or justified by
anything we found. The previous rule in this repo (class compliance first,
configuration before modules) stands. The fetch of both files was truncated
in places by the fetch tool; the claims above are limited to what was shown.

## What the profile configures, and why

| Setting | Value | Reason |
|---|---|---|
| Profile | pro-audio (global rule, section 1) | All ports as individual channels |
| `session.suspend-timeout-seconds` | 0 | No wake-up latency at the start of a take. WirePlumber default is 5 s. |
| `api.alsa.period-size` | 128 | Equals the pinned quantum, one ALSA period per graph cycle |
| `api.alsa.period-num` | 3 | PipeWire's usual ring depth; a starting value, see Unverified |
| `api.alsa.headroom` | 0 | No added latency until a sweep shows the need |

Match is on `node.name` `alsa_(input|output).usb-BEHRINGER_UMC1820.*`. The
fixture test shows the rule fires on that shape and on nothing else
(`tools/test-umc-profile.sh`, WirePlumber 0.5.8 in debian:trixie, the real
`JsonUtils.match_rules_update_properties` call that `monitors/alsa.lua`
makes). The real node name is not yet read from a unit.

**Channel names are not written into PipeWire.** Pro Audio names ports
`capture_AUX<n>` and `natural_less` plus device auto-connection depend on
that. Renaming ports would break channel order silently. The labels are
display data: `sonora::backend::channel_label(PortInfo::device, direction,
channel)`, which any picker uses and which returns "" for unknown devices so
a caller falls back to the number. `PortInfo::device` for a USB card is the
product description ("UMC1820 Pro", see multichannel-usb-interfaces.md), and
`is_umc1820` matches the substring case-insensitively. The ALSA backend
(PR #407) was not on main when this landed; it can call the same function
with its card name.

## Unverified (needs the unit)

Commands, in order, on the target machine with the interface attached
(`tools/umc-measure.sh` writes the figures into `brain/research/`):

    cat /proc/asound/card*/stream0          # channels with and without ADAT; rates
    cat /proc/asound/card*/usbid            # expect 1397:0503
    pw-cli ls Node | grep -i umc1820        # real node.name: does the rule's pattern match?
    pw-link -o | grep -i umc1820            # AUX0..AUX17 ports; order matches the table
    pw-dump | grep -E 'suspend|period'      # the rule's properties reached the nodes
    tools/umc-measure.sh --seconds 60       # channels, quantum sweep, xruns, round trip
    alsamixer -c UMC1820                    # clock selector entries

Open questions: that the real `node.name` carries the `BEHRINGER_UMC1820`
text (vendor string); that period-num 3 and headroom 0 hold at quantum 128
with zero xruns (otherwise raise headroom or period-num and record the
figure); that the S/PDIF-before-ADAT order is what the isochronous stream
carries; whether the channel count changes mid-session if ADAT lock drops.
