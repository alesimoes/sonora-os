# ADR-0102 — Crash recovery: software half closed by #359, the hardware power-pull protocol

- Status: Accepted
- Date: 2026-11-08

## Summary

The software half of `system.hardening.crash_recovery` is done and stays as
it is: `tools/crash-recovery-kill.py` SIGKILLs the engine mid-take, reopens
the session folder, and checks that the takes are repaired and that nothing
on disk before the kill is lost. #359 wires it into `ci.yml`. The item stays
open for the hardware half only: a power pull on a real installed unit (a
Behringer UMC1820 with a stick or an SSD). Its pass criteria are fixed here,
with an external witness on a second device, so the pulled disk is never its
own evidence. An agent cannot run that test, so the PR that carries this ADR
is partial and does not tick the item.

## Context

`session-format.md` ("Crash recovery") says what the format guarantees: the
session file is never torn, takes are self-describing, and each take's RIFF
and data sizes are checkpointed while it records. ADR-0009 states the loss
window: the disk thread calls `fdatasync` on every open take file on a
schedule, so a power cut loses at most that interval plus the capture ring's
depth, per track. At `kill -9` the kernel keeps what was already written, so
the loss is about one drain pass (10 ms).

A process kill proves the repair path. It does not prove the medium. A power
cut takes the kernel's page cache and the stick's own write cache with it,
and a SIGKILL test running on a host keeps both. `tools/crash-recovery-kill.py`
says so in its header: the stick's write cache stays with the hardware test.

The target is hardware we have not seen (ADR-0005: the ISO plus a persistence
partition, on unknown machines). The audio interface in the field is a
Behringer UMC1820. The installed root is ext4 with `noatime,commit=60,
errors=remount-ro` (`iso/config/includes.chroot/usr/share/sonora/install/fstab.template`):
`commit=60` lets the journal's metadata lag by up to 60 s, which is separate
from the take data the disk thread syncs.

## Decision

1. **The software half is closed; nothing changes.** `tools/crash-recovery-kill.py`
   (SIGKILL mid-take, reopen, headers repaired, nothing on disk before the
   kill lost) and its CI wiring from #359 stand. No code change is part of
   this ADR.

2. **The item stays open for the hardware half: a power pull on a real
   installed unit.** The unit is the target setup (UMC1820 attached, the
   installed image booted from a stick or an SSD). Recording runs for a set
   time at the pinned quantum and sample rate, then the power is cut at the
   plug, with no clean shutdown. Pass criteria, checked after the reboot:
   - the session opens (the orphan offer, not a refusal, is the only
     recovery prompt);
   - every take is a finished WAV, its RIFF and data sizes equal the size of
     the file it is in, and no take is cut short of its header;
   - each take holds at least the audio the external witness logged as
     flushed, minus the documented loss window: the sync interval plus the
     capture ring's depth per track (`session-format.md`, "Crash recovery";
     ADR-0009, "Crash recovery has a stated loss window").

3. **The witness is on a second device.** A second USB stick, not the pulled
   one, holds a witness log. The host side writes, for every checkpoint the
   engine reports as synced, the take name, the frame count and a timestamp,
   and `fsync`s its own log after each line. The pulled disk is never its
   own evidence: the witness copy is the one compared against.

4. **The run is the owner's or a hardware tester's.** An agent cannot pull
   power. The PR for this ADR is partial (its title says so) and the
   BACKLOG item is not ticked. The result (machine, buffer size, xrun count,
   and the pass or failure per take) goes in `brain/research/`, with the
   hardware named, when the run is done.

## Rationale

- The repair path is already tested, cheaply and in CI, by the SIGKILL
  harness. Repeating it on hardware would add nothing, so the hardware run
  tests only what the harness cannot: the medium and its cache.
- Pass criteria taken from the format spec mean the test checks a promise the
  format already makes, not a new number chosen after the run.
- A second device for the witness is the only way to know what was flushed
  without trusting the disk being pulled.
- Keeping the item open until the run keeps the claim honest: the software
  test passing does not show the stick keeps what the engine synced.

## Consequences

- The backlog item `system.hardening.crash_recovery` stays open with the
  hardware half only. Its text should say that, so no reader takes the
  SIGKILL test for the whole item.
- The hardware run needs a second stick, a witness script on the host, and a
  tester at the machine. It is scheduled by the owner; nothing in CI depends
  on it.
- If a take fails the criteria, the failure is a bug in the sync path or the
  medium, filed as a new item with the witness log attached. The loss window
  is not widened to make the run pass.
- Follow-up (separate item, not in this PR): a dm-flakey or dm-log-writes
  fault-injection case in CI. It is valuable, but it needs privileged
  containers and a kernel module, so it is not part of this ADR.

## Alternatives rejected

- **A QEMU or loop-device kill as the hardware stand-in.** The host page cache
  survives the kill and the stick's write cache is never in the path, so it
  has the same blind spot the SIGKILL test already states. It proves nothing
  about flush behaviour.
- **dm-flakey or dm-log-writes fault injection in CI, now.** It would test
  the medium's flush semantics in software, which is valuable. It needs
  privileged containers and a kernel module, and it is a separate item. It is
  named as a follow-up above.
- **Ticking the item on the SIGKILL test alone.** The test cannot see the
  stick's write cache, and that cache is the thing a power cut tests.

## Unverified

- The loss window on real flash media: the interval (`sync_seconds`, a
  placeholder of 2 s in `session-format.md`) and the cost of `fdatasync` on a
  stick while 18 tracks record are not measured.
- Whether the stick honours flush (its write cache may ignore `fdatasync`).
  Only the hardware run can answer this.
- The ext4 mount options of the installed image. The root in
  `fstab.template` is `noatime,commit=60,errors=remount-ro`. The persistence
  partition that holds the session folder is mounted by live-boot as an
  overlay (`usb-stick-layout.md`); the options of that partition in the
  installed (non-live) layout were not read here and are unverified.
  Also: `sonora-install` writes its own fstab (`noatime,errors=remount-ro`,
  no `commit=60`) instead of rendering `fstab.template`, so they disagree;
  the run records the real options from `findmnt` on the unit.
- Whether the engine reports each checkpoint to a host-side witness today. The
  witness design above assumes the engine's sync events can be read from
  outside; if they cannot, the witness needs a control-protocol event, which
  is a follow-up that needs its own contract change.
