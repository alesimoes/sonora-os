# ADR-0101 - ADR-0035 policy delivered; apt path demoted; session-open probe contract

- Status: Accepted
- Date: 2026-11-08
- Item: `system.install.updates`
- Relates to: ADR-0035 (updates on an installed system, policy), ADR-0044
  (A/B updates, the updater that replaces the apt path), ADR-0097 (A/B item
  closed by #398, gap 2 names the missing probe), ADR-0100 (installer item
  closed by #372)

## Summary

The policy ADR-0035 asks for is on main: sources pins, the kernel hold,
`apt-daily` conditioned on the flag, periodic apt off, no unattended-upgrades.
`tools/test-updates.sh` checks those files. Nothing in that policy changes.

The updater ADR-0035 promised is not built. The updater that ships is
`sonora-update` of ADR-0044 (A/B). An `apt upgrade` run inside a session slot
is lost at the next slot switch, so a second, apt-based updater would be two
mechanisms with different rollback. The apt path is demoted: it is not built,
and the updates-allowed flag `/var/lib/sonora/updates-allowed` is not created
by anything on the apt side. With the flag absent, `apt-daily` and
`apt-daily-upgrade` never run.

The gate `sonora-update apply` needs is the session-open probe
`sonora-session-open`. This ADR fixes its contract so the engine side and the
updater side can be built against the same text. An offline stick is
ADR-0044's `apply --from DIR`.

## Decision

1. **Policy files: no change.** The sources pins
   (`/etc/apt/preferences.d/sonora.pref`, Debian stable and security only, the
   rest at -1), the kernel and `rt-amd64` holds, the `apt-daily` and
   `apt-daily-upgrade` drop-ins, the periodic settings and the absence of
   unattended-upgrades stay as they are. `tools/test-updates.sh` keeps
   checking them.

2. **No apt updater.** The installed machine is updated by ADR-0044's
   `sonora-update`: `apply` (from https or `--from DIR` on a stick), `rollback`,
   `confirm`. The updates-allowed flag is created only by `sonora-update`, for
   its own run, and removed on exit (ADR-0044 item 7). Nothing creates it
   otherwise, so the apt timers stay inert.

3. **Contract of `/usr/local/bin/sonora-session-open`.** Read-only, no
   arguments, no engine IPC.
   - Exit `0`: a session is open. The updater refuses.
   - Exit `1`: no session is open. The updater may proceed.
   - Exit `2`: the state cannot be determined (the directory is missing or
     unreadable, or the probe itself fails). The updater refuses. A missing or
     erroring probe is a refusal, never a pass.
   - State file: `/run/sonora/session-open`, on tmpfs. The engine's session
     layer (the app) creates it when a session opens and removes it when the
     session closes. The probe tests only whether the file exists, after
     checking that `/run/sonora` is a readable directory.
   - A crash leaves the file behind. The probe then says `0` and the updater
     refuses. That is the safe direction; the operator clears it by a reboot
     (tmpfs), which the refusal message says.
   - The probe is never on the audio thread and never calls the engine.
   - Directory present, file absent: exit `1`. Directory missing: exit `2`,
     unless `SONORA_ENGINE_UP=1` is set, in which case `systemctl is-active
     sonora-engine.service` is consulted and an active unit gives `1`. The
     opt-in is off by default and systemctl is never called otherwise. It is
     not a pass on its own: a failed or absent systemctl stays `2`.
   - `tmpfiles.d` interaction: `/run/sonora` is created at boot by the
     `tmpfiles.d` line (follow-up below). Once it exists, "directory present"
     means the app side is provisioned, so an absent file is a real "none".
     Until that line lands the directory is missing and the probe says `2`,
     so `sonora-update apply` keeps refusing.

4. **Follow-ups, each with an owner:**
   - The app writes `/run/sonora/session-open` on session open and removes it
     on close. Owner: the DAW track (new BACKLOG line). Until it lands,
     `sonora-update apply` refuses in every case, which is the intended
     default.
   - The release signing key and `/etc/sonora-release`. Owner: the release
     publisher item.
   - The installer's two-slot layout (ADR-0044 item 1). Owner:
     `system.install.rootfs`.
   - A `tmpfiles.d` line that creates `/run/sonora` owned by the app user,
     so the app can write it. Owner: the DAW track, with the state-file line.

## Rationale

- The policy files are already correct and tested. Reopening them would churn
  a verified artefact for no gain.
- A second updater would need its own rollback, its own signature path and its
  own handling of the slot switch, and the apt-installed packages would be
  lost at the slot switch anyway. One mechanism is less to get wrong on a
  machine nobody watches.
- A file is the cheapest observable state the probe can read. It needs no
  daemon, no IPC and no lock held across processes, and a shell can read it on
  any layout. A stale file fails closed, which is the direction ADR-0044
  requires.

## Consequences

- Security fixes from Debian reach the installed machine only through a new
  Sonora slot (ADR-0044), and only when the operator acts. Same trade as
  ADR-0035.
- `apt-daily` never runs on a current build, because the flag is never
  created. This is deliberate. It is a behaviour the policy test should keep
  asserting; no test change is made here.
- Until the app writes the state file, `sonora-update apply` refuses on every
  machine, including idle ones. The refusal is a held notice for the operator,
  not the 2.5 s status line.
- A crashed session blocks updates until reboot. Accepted: the operator sees
  the notice and a reboot is cheap on an appliance.
- ADR-0035's "Decision 4" still describes the flag's owner as "the update
  tool". That tool is now `sonora-update`; the wording stays as history.

## Alternatives rejected

- **Build the apt updater as well.** Two mechanisms, two rollbacks, and apt
  changes are lost at the next slot switch. Rejected.
- **Probe the audio thread, or poll the engine over IPC.** Couples the updater
  to the real-time path and to IPC availability. ADR-0044 requires the
  updater to fail closed when the engine cannot answer; a file that the app
  writes gives that without a call into the engine.
- **A `flock` on the session directory.** A crash releases the lock, so it
  wrongly reports "no session" after a crash. A held lock is also invisible to
  a plain shell probe on a multi-process layout, which is less obvious than a
  file that can be listed.

## Unverified

- That the app can write `/run/sonora` as its user. It needs the `tmpfiles.d`
  line listed under follow-ups; no line exists on main yet.
- The probe script exists (`sonora-session-open`) and `tools/test-session-open.sh`
  exercises it against files in CI. It has not run on a booted image, and the
  `SONORA_ENGINE_UP` path has only run against a stubbed systemctl.
- A real installed update has never run. Nothing here was booted or
  exercised on hardware.
