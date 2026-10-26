# ADR-0017 — Windows plugins through yabridge: bridged VST3 only, no new path

- Status: **Accepted**, 2026-10-25 (coordinator, on the recommended default
  of each decision below, under the owner's standing authorization).
- Date: 2026-10-25
- Proposed by: plugin-engineer (Track 4 item 6, BACKLOG "Windows VST through
  yabridge").
- Amends: nothing. It uses ADR-0012's host, scan and probe as they are
  (ADR-0012, amendment of 2026-10-19, item 1 already lists `~/.vst3`, which
  covers `~/.vst3/yabridge`). It adds an optional `bridge` key to the host's
  `--list` lines, which no contract defines (Y2).
- Numbering: follows ADR-0016.

## Summary

*Navigation only; the sections below are the decision.*

- Y1: yabridge 5.1.1 (GPL-3.0-or-later, upstream tarball, SHA-256 pinned)
  on Debian trixie's Wine 10.0 (LGPL-2.1-or-later). Upstream reports Wine
  9.22 and later breaks editor embedding. Editors are out of scope
  (ADR-0010). The audio path was measured working on 10.0.
- Y2: no new discovery path. A bundle counts as bridged when it carries a
  Windows module *and* its Linux binary is yabridge's chainloader. The host
  marks it `"bridge":"yabridge"`. Since the update of 2026-10-25 the marker
  is in the probe record (version 2) and the catalog (protocol 14), shown
  as "Windows (Wine)".
- Y3: class IDs in a bundle's `moduleinfo.json` are matched in both byte
  orders, and both files are read (`Contents/Resources/` and `Contents/`).
  Without this a bridged bundle is "not installed".
- Y4: isolation holds as it is, and validation behaves differently. A
  plugin that over-writes its buffers does it inside Wine, where the trap
  cannot see it. It surfaces as a hang, and the timeout refuses it.
- Y5: CI runs fixtures only; the real bridge runs in
  `tools/test-yabridge.sh` with Sonora's own test plugin cross-built for
  Windows. CI skips that test (77).
- Y6: deferred: VST2, CLAP, appliance packaging, the deadline sign-off on
  the target. The catalog field, the prefix before the first scan and the
  run-time crash test are done (update of 2026-10-25).

## Context

Windows-only plugins are a large part of what a studio owns. yabridge
(Robbert van der Helm, GPL-3.0-or-later) runs a Windows VST2, VST3 or CLAP
plugin under Wine and presents it to a Linux host as a Linux plugin.
`yabridgectl sync` builds, for each Windows `.vst3`, a merged bundle under
`~/.vst3/yabridge/<name>.vst3`, which contains:

- `Contents/x86_64-linux/<name>.so`, a copy of
  `libyabridge-chainloader-vst3.so`. It `dlopen()`s `libyabridge-vst3.so`,
  which starts `yabridge-host.exe` under Wine and forwards every call over
  sockets and shared memory.
- `Contents/x86_64-win/<name>.vst3` (or `x86-win`), a symlink to the Windows
  module inside the Wine prefix.
- `Contents/Resources`, a symlink to the Windows bundle's own resources,
  when it has any.
- `Contents/moduleinfo.json`, the Windows original's `Contents/moduleinfo.json`
  with each class ID rewritten from COM byte order (yabridgectl
  `files.rs`, `vst3_moduleinfo.rs`, read 2026-10-25).

Nothing here is reverse engineering. yabridge and Wine are published free
software, and the plugins are the operator's own. ADR-0004 still applies to
what runs inside Wine: licence checks and copy protection are not worked
around.

## Decision

### Y1 — Versions and licences

- **yabridge 5.1.1**, the latest release (2024-11-04). The upstream binary
  tarball is pinned by SHA-256
  `d11f0307412b5566b09a75ed4aa191266db56d22f4de4d20e4fbde6296cd1a47`.
  Licence GPL-3.0-or-later. Debian does not package it.
- **Debian trixie's Wine 10.0 series** from main (`wine`, `wine64`,
  `10.0~repack-6`). Licence LGPL-2.1-or-later.
- Upstream tests on Wine Staging 9.x. Its issue #409 records that Wine 9.22
  and later breaks *editor embedding*: the editor window, cursor offsets,
  DPI. Sonora hosts no editors (ADR-0010), so that is not a blocker. The
  audio path on Wine 10.0 was measured instead (Y5): scan, probe, run,
  crash and the lying plugin all behave as Y4 says.
- yabridgectl 5.1.1's `set` subcommand panics in its argument parser
  (clap: "`path_auto`'s `ArgAction` should be one of `SetTrue`…"). Use the
  default search path, `~/.local/share/yabridge`, or `/usr/lib` on the
  appliance. Do not use `set --path`.
- Upstream has had no release since 2024-11, and its master has Wine 10
  work. Keep the pin, and change it only by an edit to this section with
  the Y5 test re-run.

### Y2 — Discovery: no new path, one marker

The VST3 roots already include `~/.vst3` (ADR-0012), and the scan walks
`~/.vst3/yabridge` with nothing added. The only thing added is that the host
says which bundles run under Wine:

- **Rule.** A bundle is bridged when `Contents/x86_64-win` or
  `Contents/x86-win` exists, *and* its Linux binary contains the bytes
  `libyabridge-vst3.so`, the library name the chainloader loads (measured in
  the 5.1.1 tarball). The Windows directory alone is not enough: the SDK's
  multi-platform bundle legitimately carries a native Windows build next to
  a native Linux one. The binary is read as bytes, up to 4 MiB; no code
  runs.
- **Output.** `sonora-plugin-host --list --json` adds `"bridge":"yabridge"`
  after `"bundle"`, and `--list --bundles-only --json` adds it after
  `"format"`. The text output gets a `bridge:` line. The key appears only
  on bridged lines, so every native line is unchanged. The engine's parser
  (`plugin_scanner.cpp`) looks keys up by name and ignores the new one.
- **Not yet.** The probe record (`contracts/plugin/probe_record.h`, record
  version 1) and `contracts/api`'s `PluginInfo` do not carry it. The
  operator cannot see yet that a plugin runs under Wine. That is a contract
  change under ADR-0007 §6, jointly owned, and the follow-up PR makes it:
  record version 2 with `bridge`, the catalog field, and a "Windows (Wine)"
  tag in the browser. Something the system knows and does not show should
  not stay that way.

### Y3 — `moduleinfo.json` in either byte order

Before this ADR, `Vst3World::module_for` read only
`Contents/Resources/moduleinfo.json` (the SDK's place, `module_linux.cpp`).
When that file did not name the class, the bundle was skipped: no code of an
unrelated plugin runs. That breaks for yabridge bundles. yabridgectl
rewrites only `Contents/moduleinfo.json`, and it symlinks `Contents/Resources`
from the Windows original, whose file names the class in COM byte order.
The host sees the class in the Linux order (measured: the Windows build of
the test plugin's State class is `vst3:534F…0009` through yabridge, the same
ID as the Linux build). The host therefore found no match, skipped the
bundle, and reported the plugin "not installed".

Decision: read both files, and match the class ID in either order. The
other order is bytes 0–3 reversed, 4–5 swapped, 6–7 swapped, the same
permutation as yabridgectl's `rewrite_uid_byte_order`. A false match costs
one bundle load that finds no class. A bundle that names the class in
neither order is still not loaded.

A side effect: the class ID is the same native or bridged (measured with
the test plugin). A session's saved reference to a plugin therefore does
not depend on which of the two is installed. Opening such a session is not
yet tested.

### Y4 — Isolation and validation, as they behave under Wine

The Y5 test measured the following:

- **Crash isolation holds as it is.** A Windows plugin whose factory
  segfaults aborts yabridge's Linux side (`signal 6`). That ends the scan
  child, which costs the bundle's own `--list` entry ("the scan crashed
  (signal 6, Aborted)"). The scan goes on. A Wine crash during a chain ends
  this host process, as the BACKLOG expected. The engine's existing
  host-fault path covers that case (ADR-0012). It is not yet exercised
  under Wine: a run-time crash test is a follow-up.
- **Validation changes form.** The host gives every bus spare channel
  pointers aimed at a trap (`kSpareChannels`). A plugin that declares 2
  channels and writes 8 is caught there. Under yabridge, the plugin writes
  into yabridge's Wine-side buffers, which the trap cannot see. What
  follows depends on Wine's heap. In the test the Liar never returned:
  `--list` killed its scan child at the timeout ("the scan hung (killed
  after 30 s)"), and `--probe` gave no answer within 30 s, which the engine
  records as `Blocked` (ADR-0012 amendment item 4). It is never trusted,
  but by the deadline, not by the trap. No case was seen of a liar that
  returns and corrupts silently. One is possible, and only a check of
  yabridge's own buffers would catch it. Recorded as a known limit.
- **One scan child per bundle.** A class that hangs under Wine costs every
  class after it in the same bundle, as natively. This is likelier under
  Wine, and it is why the test ships each Windows class in a bundle of its
  own.
- **Timeouts.** Wine prefix creation (`wineboot -i`) took about 10 s in the
  container. Once the prefix exists, a probe through yabridge took
  1.3–1.5 s, the Wine host start included. The engine's 30 s probe timeout
  and the 120 s scan timeout are enough once the prefix exists. A probe
  that runs while the first-ever Wine start is still creating the prefix
  could exceed 30 s and be cached `Blocked`. Packaging therefore creates
  the prefix before the first scan (Y6).
- **No licence-check circumvention** (ADR-0004). A plugin that needs iLok or
  an activation dialog under Wine either works or ends refused or blocked
  with its reason. Nothing in Sonora patches, emulates or bypasses it.

### Y5 — How it is tested

- **CI, every run, no Wine** (`plugins/host/CMakeLists.txt`, label
  `plugins`). Two fixture bundles laid out as yabridgectl lays them out, with
  a COM-order `Contents/Resources/moduleinfo.json`:
  - `sonora-test-bridged.vst3`: the test plugin built with the
    chainloader's library name in it. It stands in for yabridge and runs
    natively.
  - `sonora-test-multiplatform.vst3`: the plain test plugin next to a
    Windows module. It must not be marked.

  The tests are `plugin_host_yabridge_list_bundles`,
  `plugin_host_yabridge_list` and `plugin_host_yabridge_probe_com_order`.
  The last one fails without Y3 ("not installed").
- **The real bridge, on demand.** `tools/test-yabridge.sh` builds
  `tools/Dockerfile.yabridge`: the plugins image plus Wine, mingw-w64,
  Xvfb and yabridge. It runs `plugin_host_yabridge_wine`
  (`plugins/host/tests/yabridge_wine.sh`). The Windows plugins are
  Sonora's own test plugin (GPL-3.0-or-later), cross-built with mingw-w64,
  one class per bundle (State, Liar). No third-party Windows binary is
  downloaded. Where Wine, yabridge or mingw-w64 is missing, the test exits
  77 and ctest reports it skipped, which is CI's plugins image. CI does not
  install Wine: it would add several hundred MB and minutes to every
  plugins run for one test.
- **Measured, 2026-10-25, container figures** (Docker Desktop, shared
  kernel, no PREEMPT_RT; not appliance figures). One mono pass-through, 3000
  blocks of 128 at 48 kHz, futex wake:

  | | p50 | p99 | p99.9 | max | dropped | host worst block |
  |---|---|---|---|---|---|---|
  | native (Linux build) | 58 µs | 250 µs | 456 µs | 509 µs | 0 | 26 µs |
  | through yabridge + Wine 10.0 | 161 µs | 598 µs | 755 µs | 793 µs | 0 | 547 µs |

  yabridge's hop costs about 100 µs at the median and 350 µs at p99 per
  plugin chain. That is inside the driver's 2000 µs deadline, of the
  2667 µs period. These are container figures.

### Y6 — Deferred, with the recommended default

- **VST2: not hosted.** yabridgectl also makes `.so` VST2 bridges under
  `~/.vst/yabridge`; the host does not scan `~/.vst`. A VST2 host needs
  Steinberg's VST2 SDK, whose licence is no longer granted, or a clean-room
  header. The clean-room headers (`vestige`, `FST`) are published under
  free licences and so are within ADR-0004. They are, however, reverse
  engineered from a licensed SDK, with a legal position nobody has tested.
  Recommended default: no VST2. Most Windows plugins sold today ship VST3.
  Revisit only if the owner asks.
- **CLAP**: neither native nor bridged CLAP is hosted. That is a separate
  item.
- **Appliance packaging** (after the product, per the roadmap): Wine and
  the yabridge files in the image (`/usr/lib`, where yabridgectl looks), a
  prefix created at first boot before the first scan (Y4), and the licences
  recorded with the image's others. The plugins image itself stays free
  of Wine.
- **Offered to the operator only after the target measurement.** The
  BACKLOG requires the hop to be measured against the 2.67 ms deadline
  before it is offered. The table above is a container figure. The
  sign-off is the same run on the appliance kernel with the UMC1820, plus
  at least one real Windows plugin. Unverified: whether yabridge's Wine
  audio threads get real-time priority there.
- **The catalog field** (Y2, "Not yet").

## Consequences

- A Windows VST3 that yabridgectl bridged is found, probed, validated and run
  by the same host, with the same crash and hang isolation. The new code is
  the marker and the byte-order match.
- "Validate before trusting" relies on the deadline for bridged plugins, as
  Y4 records. A bridged plugin that lies and still returns in time would not
  be caught.
- The test image (`sonora-yabridge:trixie`) is local and labelled
  `org.sonora=1`. `tools/clean.sh` removes it with the rest.

## Update, 2026-10-25: the Y2, Y4 and Y6 follow-ups

Made by the follow-up PR (BACKLOG Track 4 item 6). The appliance deadline
sign-off (Y6) is still open: it needs the UMC1820.

- **The marker reaches the operator (Y2).** The probe record is version 2:
  `bridge` comes after `bundle`, only on a bridged VST3, and its only value
  is `"yabridge"`. `decode()` still reads version 1, as a native plugin's.
  The engine's cache is discarded anyway, because the host's `--version`
  line now names `probe 2`. `contracts/api` `PluginInfo::bridge`
  (`PluginBridge::None | Yabridge`, after `has_editor`) is control protocol
  14. The chooser and the insert panel show "Windows (Wine)". A Blocked
  record, which the engine writes itself, takes the marker from the
  bundle's listing. A Windows plug-in that hung is still shown as one.
- **The prefix is created by the scanner, not only by packaging (Y4,
  Y6).** When a scan lists a bridged bundle and `<prefix>/system.reg` is
  missing, the scanner first runs `env WINEDLLOVERRIDES=mscoree,mshtml=
  wineboot --init` as a child of its own, with a 120 s limit. Nothing
  bridged is enumerated, probed, or started for a chain until that child
  ends. The prefix is `$WINEPREFIX`, or `~/.wine` when it is unset, resolved
  by the entry layer. Packaging can still create the prefix at first boot;
  the scanner then finds it and does nothing. Doing it in the scanner too
  covers a deleted prefix and a developer's machine. If the prefix cannot
  be made (no Wine, a failure, the limit), the operator is told. Nothing
  bridged is probed or cached Blocked, and the next scan tries again. The
  Mono and Gecko installers are off because their dialogs would wait for
  an answer on a kiosk where nobody can give one.
- **A crash during a running chain (Y4).** `yabridge_wine.sh` step 7 runs
  the Crash class, built for Windows, after the State pass-through through
  the plugin boundary (`plugin_driver --scenario crash_bridged`) and
  triggers the segfault inside Wine. The crash is in Wine's process. The
  host may die, if yabridge aborts, or stop answering. The scenario accepts
  either, because both are faults the engine already handles (ADR-0012
  §3). What must hold: every callback runs, none waits on Wine, nothing
  completes from the crashed chain, and the Crash class is the plugin
  named. Wine's crash dialog is turned off in the test prefix, for the same
  kiosk reason as the installers; packaging must do the same. The test
  runs only under `tools/test-yabridge.sh` (CI skips it, 77). Which of the
  two outcomes Wine 10.0 and yabridge 5.1.1 give is not yet measured.
