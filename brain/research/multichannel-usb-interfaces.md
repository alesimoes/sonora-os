# Multichannel USB interfaces on the appliance

Context: the owner intends to connect a Behringer UMC-series interface. This
note records what was established, and is explicit about what was not.

## Verified here, by reading the shipped code

Read from WirePlumber 0.5.8 as packaged in Debian trixie, inside the runtime
container on 2026-10-11.

**Profile selection.** `scripts/device/find-preferred-profile.lua` reads a
`device.profile.priority.rules` section and expects the matched rule to set a
`priorities` property holding an array of profile names. The first name the
card actually offers wins; if none match, the hook declines and WirePlumber
falls back to its own selection. Our rule lives in
`audio-stack/wireplumber-sonora.conf`.

**Stored state outranks preference.** `find-best-profile.lua` declares
`after = "device/find-preferred-profile"`, and the stored-profile hook runs
earlier still. A profile WirePlumber remembered from a previous run therefore
beats our preference. `wireplumber.conf` ships a `mixin.stateless` block whose
own comment says it is "for embedded systems that should always reboot to a
default state", disabling four hooks:
`hooks.device.profile.state`, `hooks.device.routes.state`,
`hooks.default-nodes.state`, `hooks.stream.state`. We disable those four
rather than adopting the ready-made `main-embedded` profile, because that
profile also pulls in `mixin.systemwide-session` and ours runs in the user
session with lingering.

**Rule syntax.** Taken from the one example Debian ships,
`wireplumber.conf.d/alsa-vm.conf`: a list of objects with `matches` (a list of
constraint objects, `~` prefixing a regex) and `actions.update-props`.

**Port ordering.** `jack_get_ports` documents no ordering. A lexicographic
sort places `capture_AUX10` before `capture_AUX2`, which on an 18-channel
interface records channel 10 onto track 3 with nothing looking wrong. Hence
`sonora::natural_less` in `engine/include/sonora/port_order.h`, with the
18-channel case as a test in `engine/tests/test_port_order.cpp`.

## Reasoned, not verified

The Pro Audio profile is the standard way to get per-channel ports out of a
multichannel card, and the default profile presents a stereo pair. This is
consistently documented by PipeWire and by every multichannel-interface guide,
and it is why the rule above exists. We have not observed it on a UMC, so
treat the specific claim "a UMC1820 appears as 2 channels without this" as
expected behaviour rather than a measurement.

## Not established at all

- **Which model.** UMC channel counts differ enormously: a UMC204HD has 2
  inputs, a UMC404HD has 4, a UMC1820 has 10 analog plus ADAT for 18 over USB.
  Nothing in the project should assume a number, which is why
  `--tracks auto` exists and sizes the session from the ports the server
  offers.
- **Whether the class-compliant path needs any quirk.** These interfaces are
  USB audio class compliant and should need no driver, but we have not checked
  whether this one needs `snd-usb-audio` module options, nor what the lowest
  stable quantum is on it.
- **Any latency or xrun figure.** None. We have no measurement from any real
  interface. Every number in this repository so far comes from a container
  sharing the host kernel, which says nothing about real-world latency.

## What would settle it

Boot the image on the target machine with the interface attached, then record:

    uname -r                        # must contain -rt-
    pw-cli list-objects | grep -i behringer
    pw-link -o | grep capture       # how many channels, and their names
    sonora-engine --tracks auto --arm-all --record 60 --wait-for-inputs 60

The engine prints `RESULT ... connected=N ... xruns=N overruns=N`. Those
numbers, with the model name and the quantum, belong in this file.
