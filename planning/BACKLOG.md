# Backlog

Ordered within each phase. An item is ready when someone could start it
without asking a question.

## Phase 1

- [ ] Compile the engine on Debian trixie and fix what the compiler finds.
- [ ] Run `ctest`; make the ring buffer and BWF tests pass.
- [ ] `live-build` config: package list, RT kernel, PipeWire from backports.
- [ ] `sonora-session.service`: autologin on tty1 into the engine, respawn on
      exit, no display manager.
- [ ] PipeWire drop-in pinning quantum to 128 and rate to 48000.
- [ ] `limits.d` granting `rtprio 95` and `memlock unlimited` to the audio
      group.
- [ ] Udev rule naming the first class-compliant USB interface so port
      auto-connection is stable across reboots.
- [ ] `tools/write-usb.sh`: write the ISO and create the persistence partition.
- [ ] Boot test on real hardware; record the xrun count in
      `brain/research/`.
- [ ] QEMU boot smoke test in CI. It must assert three things, because each
      one has already broken once and only the first is obvious:
      the system reaches `multi-user.target`; `sonora-engine` is running;
      and `uname -r` contains `-rt-`. An image that boots and records on the
      wrong kernel passes any looser test.

## Blocked on hardware the project does not have

- [ ] Confirm the Pro Audio profile actually exposes every channel on the
      owner's Behringer UMC, and record the model and channel count in
      `brain/research/multichannel-usb-interfaces.md`.
- [ ] First real latency figure: lowest quantum that holds with zero xruns on
      that interface, on the RT kernel.

## Phase 2

- [ ] Playback path: read from disk into a ring, mix, feed the output ports.
- [ ] Input monitoring with latency compensation measured by a loopback test.
- [ ] Transport locate without dropping a block.
- [ ] Timeline data model: regions over sources, with an undo stack.
- [ ] Session file format spec, then the reader and writer.

## Known gaps, not yet scheduled

- RF64 for files over 4 GB. The 32-bit RIFF size field overflows at about
  8 hours of 24-bit mono at 48 kHz. See `brain/specs/bwf-broadcast-wave.md`.
- `DiskThread` polls every 10 ms. Correct, but a condition variable signalled
  by the callback would be better once there is a reason to care. It must not
  make the callback block.
- The disk thread writes with buffered `stdio`. Direct I/O with aligned buffers
  avoids double-buffering through the page cache; worth measuring before
  changing.
- No RT priority is requested for the audio thread yet; PipeWire sets it for
  its own client thread, which is enough until we own threads.
