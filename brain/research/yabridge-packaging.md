# yabridge and Wine in the image: provenance, licences, RT notes

Checked 2026-11-04 (primary sources: the upstream release page and Debian's
package tracker as recorded in ADR-0017 Y1). Nothing here was run on
hardware; see "Unverified".

## What ships and where it comes from

| Part | Version | Source | Licence | Integrity |
|---|---|---|---|---|
| Wine (`wine`, `wine64`) | trixie 10.0 series (`10.0~repack-6` when ADR-0017 was written) | Debian main, from the image's dated snapshot | LGPL-2.1-or-later | apt signatures; the hook refuses any other series |
| yabridge, yabridgectl | 5.1.1 | `https://github.com/robbert-vdh/yabridge/releases/download/5.1.1/yabridge-5.1.1.tar.gz` | GPL-3.0-or-later | SHA-256 `d11f0307412b5566b09a75ed4aa191266db56d22f4de4d20e4fbde6296cd1a47`, checked by `sha256sum -c` in `iso/config/hooks/normal/0200-sonora-yabridge.hook.chroot` |

Debian does not package yabridge. The tarball is a binary release of the
upstream author; the pin is the same one `tools/Dockerfile.yabridge` uses for
the container test, and `tools/test-image-config.sh` fails if the two differ.
Wine Staging is not used: Debian does not ship it, and a package from outside
the snapshot would make the image unreproducible (ADR-0002). The audio path on
plain Wine 10.0 is what ADR-0017 Y5 measured.

yabridge is GPL-3.0 and is not linked into Sonora: the plugin host loads the
bridged `.vst3` as a plugin, the same arm's-length relation as any VST3
(ADR-0017 Y1). The image's licence text for it is
`/usr/share/doc/yabridge/SONORA-PROVENANCE` plus the upstream tree in
`/opt/yabridge`.

Only 64-bit Wine is installed: no `wine32`/i386 multiarch, so 32-bit
Windows plugins and `yabridge-host-32.exe` are unsupported. VST2 bridges
(`yabridgectl` also writes `~/.vst/yabridge`) are not scanned (ADR-0017 Y6).

## How it is set up on the installed system

1. Build: the package list `windows-plugins.list.chroot` adds Wine; hook 0200
   fetches and verifies yabridge into `/opt/yabridge`, copies
   `libyabridge-*.so` to `/usr/lib` and links `yabridgectl` and
   `yabridge-host.exe` into `/usr/local/bin`.
2. First boot: `sonora-windows-plugins.service` (oneshot, user `sonora`,
   `Nice=15`, idle I/O, not ordered before the engine) runs
   `sonora-windows-plugins`: `wineboot --init` into `~/.wine` (the prefix the
   plugin scanner also uses, so there is one), Wine's crash dialog off, then
   `yabridgectl add` for `~/Windows-Plugins` and the prefix's
   `Common Files/VST3`, and `yabridgectl sync`. A stamp in
   `~/.local/state/sonora/` keeps the unit to once.
3. On demand: the operator copies Windows `.vst3` bundles into
   `~/Windows-Plugins` and runs `sonora-windows-plugins` again (or any
   folder as argument). `--check` prints the plan and changes nothing.
4. Scan: `yabridgectl sync` writes Linux bundles to `~/.vst3/yabridge`; the
   host already scans `~/.vst3` (ADR-0012) and marks them `bridge: yabridge`
   (ADR-0017 Y2). No scanner change is needed.

If the scanner runs before the unit has finished, it creates the prefix
itself (ADR-0017 Y4) and the bridged bundles appear at the next rescan.

## Real-time notes

- Wine threads are ordinary `SCHED_OTHER` unless asked. Debian's plain Wine
  has none of Wine Staging's `STAGING_RT_PRIORITY_*` switches. What yabridge
  does itself (upstream documentation, not re-measured here): it raises the
  scheduling priority of the plugin's audio thread in the Wine host to match
  the calling host thread, which needs the `rtprio` rlimit of the process that
  starts it. The plugin host is started by the engine, which runs under
  `LimitRTPRIO=95` (`sonora-session.service`) and inherits the limit
  through `limits-audio.conf`, so the grant is available.
- `sonora-windows-plugins.service` deliberately has no RT limit and runs at
  `Nice=15`: it must never compete with the engine while a take is recording.
- The extra IPC hop is a deadline cost, not a priority one: p50 161 us, p99
  598 us per block round trip in the container (BACKLOG Track 4 item 6),
  against the 2.67 ms deadline at 128 frames and 48 kHz. Bridged plugins are
  not offered until the appliance run below.

## Unverified (owner, with a Windows VST3 and the target machine)

No Windows plugin was available, and the ISO was not built or booted for this
change. Commands, on the installed system (UMC1820 attached):

    sudo apt-get install --reinstall wine64   # only if the image is older
    wine --version; yabridgectl --version     # 10.0.x; 5.1.1
    mkdir -p ~/Windows-Plugins && cp -r /path/Plugin.vst3 ~/Windows-Plugins/
    sonora-windows-plugins --check
    sonora-windows-plugins
    ls ~/.vst3/yabridge
    sonora-plugin-host --list --json | grep yabridge
    # in a take at 128 frames / 48 kHz, with the plugin inserted:
    ps -eLo cls,rtprio,comm | grep -i -E 'wine|yabridge'   # FF/RR rows = RT granted
    journalctl -u sonora-session -b | grep -i xrun

Record the machine, the buffer size and the xrun count here when it is run.

## Appliance sign-off run (ADR-0139, owner's hardware command)

The deadline sign-off is run on the installed appliance with the UMC1820
attached and a real Windows VST3 in `~/Windows-Plugins`. It needs the built
`plugin_driver` and the test crash plugin (Windows build) in
`SONORA_YABRIDGE_TEST_VST3_DIR`. Command, from the Sonora checkout:

    SONORA_YABRIDGE_TEST_VST3_DIR=/path/to/test-vst3 \
    tools/signoff-yabridge.sh --host /path/to/sonora-plugin-host \
        --driver /path/to/plugin_driver \
        --plugin ~/Windows-Plugins/Plugin.vst3 \
        --blocks 225000 --report ~/sonora-yabridge-signoff.txt
    echo "exit $?"

`--blocks 225000` is the 10-minute run at 128 frames and 48 kHz (ADR-0139,
criterion 3). Exit 0 is PASS, 1 is FAIL (the report names the criteria), 77
is SKIP (Wine, yabridge or the plugin absent). A skip is not a sign-off. Keep
the report file with the take log and attach it to the BACKLOG item.

Record here: the machine, the Wine and yabridge versions, the report's
round-trip line, the dropped count, the RT thread count, the crash outcome
(`died` or `silent`) and the exit code. Run it once with the plugin as
shipped and once under load (a second session open), and record both.

Open for the researcher, to settle from that run (ADR-0139, Consequences):

- Wine's audio threads at a real-time class under yabridge 5.1.1 on the
  appliance: which `rtprio`, and whether short-lived threads are missed by the
  once-per-second `ps` sampling.
- How a crashing bridged plugin ends (died or silent) and how long the silence
  lasts before the timeout refuses it.
- Whether 667 us between the deadline (2000 us) and the period (2667 us) is
  enough margin for the drop rate and the IPC jitter.
