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

## Which name auto-connection depends on, and why no udev rule is needed

Asked by the Phase 1 item "udev rule naming the first class-compliant USB
interface so port auto-connection is stable across reboots". Read from
PipeWire 1.4.2 and WirePlumber 0.5.8 as trixie ships them, and measured in
the runtime container on 2026-11-01. Not booted, and no udev runs in the
container, so nothing here touched a USB device.

**The chain, from the kernel to the engine's device name.**

1. `spa/plugins/alsa/alsa-udev.c` (1.4.2, lines 591-603) sets
   `device.product.name` from the udev properties `ID_MODEL_FROM_DATABASE`,
   else `ID_MODEL_ENC`, else `ID_MODEL`. All three come from the USB
   descriptor or hwdb, never from the card index. `ID_PATH` and `ID_ID` give
   `device.bus-path` and `device.bus-id`, which are stable too.
2. `scripts/monitors/alsa.lua` (0.5.8, `prepareDevice`) sets
   `device.description` to `device.product.name` when the device has none,
   and `createNode` (lines 278-293) makes each node's description
   `"<device.description> <device.profile.description>"`, hence "UMC1820
   Pro".
3. `pipewire-jack/src/pipewire-jack.c` (1.4.2, lines 3738-3767) names the
   JACK client part of every port after `node.description` (`node.nick`
   only with `jack.short-name`).
4. `describe_ports()` (`drivers/support/include/sonora/port_devices.h`)
   takes the device as the client part, and ADR-0007 item 8 selects by it,
   by USB-ness (`/proc/asound/<id>/usbid`, from the alias), and by channel
   order within it.

Nothing on that path reads the ALSA card index (`cardN`, `hw:N`), which is
the one name that changes with enumeration order. Renaming the card id
through udev (`ATTR{id}`) would change only the `alsa:acp:<id>` alias, and
`alsa_card_is_usb()` follows `/proc/asound/<id>` whatever the id is. So with
one interface the name auto-connection uses is already the same on every
boot, and a naming rule would change no behaviour. The image ships none.

**What is not stable: two devices with the same description** [measured].
Two capture nodes sharing `node.description` (`support.null-audio-sink`,
`port.physical=true`), first from one client as WirePlumber creates ALSA
nodes, then from two clients. `jack_get_ports(..., JackPortIsOutput)` gave
in both cases:

    Dup Pro:capture_AUX1
    Dup Pro:capture_AUX1-48

pipewire-jack keeps one client name and makes the second node's port unique
by appending `-<port global id>`. Global ids are handed out in creation
order, so the suffix, and which unit gets the bare names, can change from one
boot to the next. `describe_ports()` then sees one device with both units'
ports, and `natural_less` interleaves them (`AUX0`, `AUX0-<id>`, `AUX1`,
...): with two UMC1820s, track 2 records the second unit's channel 1 with
nothing looking wrong. A udev rule cannot fix this on its own: it would have
to make the description unique, and only a serial (which Behringer units may
not report) or the USB port path (which changes when the cable moves) can.
The fix belongs in the engine: a device whose ports carry that suffix is two
devices, and is refused or split. Not verified on two real interfaces.

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
