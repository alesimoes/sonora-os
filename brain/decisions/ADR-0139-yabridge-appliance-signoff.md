# ADR-0139 - Appliance sign-off criteria for bridged Windows plugins

- Status: Proposed
- Date: 2026-11-09
- Item: `plugins.formats.yabridge`, Y6 deadline sign-off (ADR-0017, Y6)
- Relates to: ADR-0017 (Y4 isolation, Y6 deferred sign-off), ADR-0012
  (host, scan and probe), ADR-0008 (portability; the script is a shell
  tool, not engine code)
- Script: `tools/signoff-yabridge.sh`, criteria in `tools/lib/yabridge-signoff.sh`
- Numbering: 139 is reserved by the coordinator for this item (next free after ADR-0133)

## Context

ADR-0017 bridges Windows VST3 plugins through yabridge under Wine. The
Windows-plugin offer to the operator is still gated: "the deadline sign-off on
the appliance with a real Windows plugin, and whether Wine's audio threads get
RT priority there" (ADR-0017, update of 2026-11-04). Nothing in the engine
measures a bridged plugin against the callback budget on the target machine.

The budget is fixed by the engine. At quantum 128 and 48 kHz a block is 2667 us
(`YB_PERIOD_US`). The plugin boundary drops a block that has not returned by
2000 us (`YB_DEADLINE_US`, ADR-0017 Y4). A bridged plugin crosses a process
boundary, so its round trip is IPC plus Wine plus the plugin's own work. A
round trip that exceeds the deadline is a dropped block for that track, and a
plugin that keeps doing it is an audible hole, not a slow plugin.

Three things are unknown until the hardware run: the round-trip distribution
of a real Windows VST3 under Wine on the UMC1820 target, whether Wine's audio
threads run at a real-time scheduling class, and what a crashing bridged plugin
does to the session (ADR-0017 Y4 says a hang, and the timeout refuses it).

## Decision

The offer of Windows plugins (the chooser entry, the "Windows (Wine)" label
as a usable plugin, not only a marker) is made only when the appliance sign-off
holds. The criteria below are the sign-off. Each number is the constant the
script uses; the script exits 0 only when every criterion holds.

Criteria, measured on the installed appliance with a real Windows VST3, at the
real period (128 frames, 48 kHz), for a 10-minute run:

1. **Round trip inside the deadline.** The bridged round trip has p99.9 at or
   below `YB_DEADLINE_US` = **2000 us**. The margin is the gap to the period:
   2667 - 2000 = 667 us. The script's pass line is "p99 inside the deadline";
   see Consequences for the difference to the p99.9 rule.
2. **Worst block inside the period.** The maximum round trip is at or below
   `YB_PERIOD_US` = **2667 us**.
3. **No overruns.** Zero blocks dropped over the run. A 10-minute run at
   128 frames and 48 kHz is **225000** blocks (600 s / 2.667 ms). The script's
   default `BLOCKS=20000` is about 53 s and is not a sign-off run; the sign-off
   passes `--blocks 225000`.
4. **Wine audio threads at an RT class.** At least one Wine or yabridge thread
   runs `SCHED_FIFO` or `SCHED_RR` with `rtprio` above 0 in
   `ps -eLo cls,rtprio,comm`, sampled during the run. This is an assumption to
   settle by the hardware run (see below).
5. **Crash recorded and contained.** `crash_bridged` ends with the bridged host
   dead (`the host died at block`) or silent (`the host stopped answering`), the
   callbacks kept running, and the driver exits 0. The outcome (`died` or
   `silent`) is written to the report. The outcome itself is recorded, not
   judged: the criterion is that the engine survived it.

Failure. A failed run, a skipped run (exit 77: Wine, yabridge or the plugin
absent) or a run not yet made leaves the state as it is today:

- The label stays **Windows (Wine)**. It is not renamed to hide the bridge.
- The chooser keeps the entry marked **unverified**. The operator sees the
  report's failed criteria, not a silent change.
- No silent degrade: the engine does not quietly lower the buffer size, drop
  the plugin from the chain, or switch the plugin to a different host without
  telling the operator. A plugin that misses its deadline during a session is
  dropped for that block and listed, as ADR-0017 Y4 and CLAUDE.md require.

## Rationale

- The numbers are the engine's own. A sign-off that used a looser deadline than
  the one the boundary enforces would pass plugins the engine then drops.
- p99.9 is the criterion the owner set for a bridged plugin, because a 10-minute
  run at 375 blocks per second has about 225 samples above p99.9. A p99 rule
  alone hides a tail that drops a block every few seconds.
- A run-length number is written down so that "10 minutes" means one thing on
  every machine. 225000 blocks is the computed value; a run of fewer blocks is
  not the sign-off.
- The sign-off is a measurement on the target machine. The result is evidence
  for an operator decision, and a failure is a labelled state, not an error.
- Recording the crash outcome, instead of demanding one, keeps the criterion
  about the session surviving. Whether a plugin dies or goes silent is a fact
  about that plugin, and the operator is entitled to it.

## Consequences

- Until the appliance run passes, Windows plugins stay "Windows (Wine)
  unverified" in the chooser. ADR-0017's Y6 stays open in the BACKLOG.
- The sign-off script's round-trip check is p99.9 against the deadline, as
  criterion 1 says, and its default `BLOCKS` is 225000. The constants agree:
  `YB_PERIOD_US=2667`, `YB_DEADLINE_US=2000`. A script pass on the target
  machine is the sign-off.
  *Amendment, 2026-11-13: the earlier known gap (p99 checked, `BLOCKS` 20000)
  is resolved; the script was aligned in PR #631.*
- The sign-off is per machine and per plugin. A pass on the UMC1820 appliance
  does not clear another interface or another plugin.
- Wine's RT behaviour may differ between Wine versions. ADR-0017 Y1 pins Wine
  10.0; a change of Wine version reopens criterion 4.
- Open for the researcher, settled only by the hardware run:
  - **RT priority under Wine (criterion 4).** Whether Wine's audio threads get
    `SCHED_FIFO`/`SCHED_RR` on the appliance, and at what `rtprio`. The
    criterion assumes a thread at RT is enough. The sampler at once per second
    may miss short-lived threads (signoff-script follow-up).
  - **Crash outcome (criterion 5).** Whether a crashing bridged plugin
    dies or goes silent under yabridge 5.1.1, and how long the silence lasts
    before the timeout refuses it. The criterion records either, so the run
    decides what is shown, not whether the criterion holds.
  - **Margin (criterion 1).** The 667 us gap to the period is the margin this
    ADR assumes. Whether 667 us is enough for the drop rate and the IPC jitter
    is a measurement, not a constant.

## Alternatives rejected

- **A code gate in the engine that refuses bridged plugins.** The engine would
  check the marker `"bridge":"yabridge"` and refuse to load any bridged
  plugin, whatever the machine. Rejected because the question is not whether
  bridged plugins are allowed in code but whether this machine can run them
  inside the budget. A code gate refuses a plugin that would pass on the
  appliance, and it passes one that would fail on a slower machine that is
  not checked. It also puts a Wine-specific rule in the audio path's
  configuration, which ADR-0017 Y4 keeps out of the engine. The sign-off is a
  measurement; the gate would be a guess.
- **Offering Windows plugins on Wine presence alone.** Rejected: presence says
  the plugin loads, not that it meets the deadline. ADR-0017 Y4 already says a
  plugin that over-writes its buffers in Wine surfaces as a hang, and only a
  run shows that.
- **Relaxing the deadline to the period (2667 us) for bridged plugins.** A
  block that returns at 2600 us misses the engine's deadline and is dropped.
  Rejected: the period is the hard edge, the deadline is the budget the boundary
  keeps.
- **Silently dropping a plugin that fails sign-off, or changing its label.**
  Rejected: the operator must see the failure. A quiet change looks like a
  working plugin in a session that is not.

## Update rule

Changing a constant or a criterion is an amendment to this ADR, dated, with the
hardware run that justified it attached. The script constants and this ADR must
be changed in the same PR.
