# Multichannel USB interfaces on the appliance

Context: the target interface is a **Behringer UMC1820**, confirmed by the
owner on 2026-10-11. USB audio class compliant, so no vendor driver is
involved. This note records what was established, and is explicit about what
was not.

A UMC1820 is the awkward case rather than the easy one: it is a high
channel-count device, and every assumption that holds for a stereo interface
fails on it quietly. All three of the engine fixes below exist because of it.

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

## Verified by booting the image

Measured 2026-10-11 in QEMU with an emulated `intel-hda` card, via
`tools/test-boot-qemu.sh`. The rule works: WirePlumber selected the Pro Audio
profile, and the node names carry it.

    alsa_input.pci-0000_00_04.0.pro-input-0:capture_AUX0
    alsa_input.pci-0000_00_04.0.pro-input-0:capture_AUX1

`pro-input-0` is the profile showing up in the node name, and `capture_AUX<n>`
is the per-channel port naming the engine sorts with `natural_less`. Without
the rule these would have been a stereo `analog-stereo` node instead.

So the *mechanism* is no longer an assumption. What it was tested against is a
two-channel emulated device, which proves the profile is applied and the port
naming is what we expect, and proves nothing about channel count.

## Reasoned, not verified

That a UMC1820 presents as a stereo pair *without* the rule, and all 18
channels *with* it, remains expected behaviour rather than a measurement. The
profile mechanism is confirmed; the device is not.

## Not established at all

- **The actual channel count the device presents.** The UMC1820 is specified
  as 18 in / 20 out over USB, combining its analog inputs with ADAT. How many
  capture ports PipeWire exposes in the Pro Audio profile, and whether that
  number changes with the ADAT clock state or the sample rate, has not been
  observed. `--tracks auto` exists so the project never has to assume it.
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
