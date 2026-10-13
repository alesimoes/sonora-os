# VST3 hosting in the plugin host: licence, plugins, measurements

What adding VST3 to `sonora-plugin-host` (plugins/host) showed on
2026-10-13. Branch `feature/plugin-host-vst3`. Same machine, container,
privileges and caveats as `plugin-host-lv2-container.md`: **every timing
here is a container figure, not an appliance figure.** The engine-facing
boundary (`contracts/plugin`, still a DRAFT) did not change layout. A chain
holds LV2 and VST3 plugins together, in one host process.

## Licence [checked 2026-10-13, primary sources]

| Source | What it says |
|---|---|
| Steinberg press release, 2025-10-29, "VST 3 Now Available Under MIT License" ([PDF](https://ocl-steinberg-live.steinberg.net/_storage/asset/819253/storage/master/Press%20Release%20-%202025-10-29%20-%20VST%203.8%20-%20EN.pdf)) | "By releasing the VST 3.8 SDK, Steinberg transitions to the MIT Open Source License, marking a significant shift from the previous dual licensing model." Use, modification and distribution are allowed if the copyright and licence text are kept. |
| [`LICENSE.txt` in steinbergmedia/vst3sdk](https://github.com/steinbergmedia/vst3sdk/blob/master/LICENSE.txt), master, read 2026-10-13 | MIT License, copyright Steinberg Media Technologies GmbH. |
| Debian trixie `libvst3sdk-dev` 3.7.10.14+~4.13.3-4, `pluginterfaces/LICENSE.txt` | Pre-MIT dual licence: "the Steinberg VST3 License, or alternatively ... the General Public License (GPL) Version 3". `debian/copyright`: `Files: *` GPL-3; `base/*`, `public-sdk/*`, `vstgui4/*` BSD-3-clause. |

**Verdict: GPL-3 compatible either way.** The build uses only the
`pluginterfaces` headers from Debian's 3.7.10 package, under its GPL-3 arm.
The MIT 3.8+ SDK is also compatible, so a later move to it needs no new
licence analysis. This settles the SDK part of ADR-0011 Unverified 1.

The "VST" name and logo are trademarks. Their use falls under Steinberg's
separate usage guidelines, not under the code licence. The docs say
descriptively that the host "loads VST3 plugins" and nothing more
(ADR-0004).

## Plugins used [measured]

| Package (trixie main) | Class | ID | Latency at 48 kHz | Licence |
|---|---|---|---|---|
| `vst3sdk-plugins` 3.7.10 | AGain VST3 (Steinberg sample) | `vst3:84E8DE5F92554F5396FAE4133C935A18` | 0 | BSD-3-clause (`public.sdk`, per `debian/copyright`) |
| `lsp-plugins-vst3` 1.2.21 | LSP Limiter Mono | `vst3:6473702062316D20202020206A7A357A` | 240 at defaults | LGPL-3+ |
| in tree, `plugins/host/tests/vst3` | crash, stereo-only, yes-man, liar, turncoat, params, latency | `vst3:534F4E4F5241565354335445535400{01..07}` | 0 (latency: 0, then 64) | GPL-3.0-or-later (ours) |

The task suggested Surge XT and Airwindows Consolidated. Neither is in
Debian trixie main, and building them in the image costs minutes per CI run.
Also packaged and unused: `dpf-plugins-vst3`, `zam-plugins`,
`dragonfly-reverb-vst3`.

## Results [measured, `tools/build-plugins.sh`: 72/72 ctest, 19 of them VST3]

- **Gain.** AGain's ParamID 0 is a linear factor, normalised 0..1. Set
  through the parameter ring to 1, 0.501 and 0.251, it measured +0.0000,
  −6.0000 and −12.0000 dB. This holds mono and stereo (AGain takes either
  arrangement, so one instance runs, not replicas), at depth 1 and depth 0.
  A change pushed in callback p lands in exactly the first block submitted
  after it.
- **Latency is what the plugin does, to the sample.** The LSP limiter
  reported 240 frames, plus 128 of IPC at depth 1 = 368. The measured delay
  was 368, with residual 0 at L and 0.199 at L±1 (poll-only: 240 and 240). A
  mixed chain of x42-dpl (LV2), the LSP limiter (VST3) and AGain (VST3)
  reported 64 + 240 + 0 + 128 = 432 and measured 432.
- **Latency changing while running.** The LSP look-ahead was moved from
  normalised 0.738 to 0.5 in block 300. The report went from 368 to 195
  frames with a new `latency_generation`, 13 blocks (35 ms) after the push
  when the processing thread read it; 10 to 12 blocks (27 to 32 ms) in five
  runs once the main thread read it (Finding 4). The measured delay after it
  settled was 195, nulling exactly. The in-tree latency plugin, which tells
  the host 5 ms after its change, was reported 4 blocks after the push.
- **Crash.** In a chain of AGain then the in-tree crash plugin, the host
  died of SIGSEGV 11 blocks after the trigger. The driver named chain
  index 1, and no callback overran the period, at depth 1 and depth 0.
- **Refusals**, all before Ready:
  - stereo-only plugin on a mono chain: `ChannelMismatch`;
  - plugin that answers `kResultTrue` to mono but keeps stereo:
    `ChannelMismatch`;
  - plugin that declares 2 channels and writes 8: `WroteOutsideBuffers`;
  - plugin that takes stereo and answers 8 channels once its bus is
    activated: `ChannelMismatch`. It used to segfault the host, which laid
    out channel pointers from the fresh answer and indexed past the
    2-entry chain buffers;
  - unknown class id: `NotFound`, naming instance 1.
- **Mono replication.** `--probe` of the LSP Limiter Mono on 2 channels
  gives 2 replicas and latency 240.
- **Scan.** `--list --format vst3` over `/usr/lib/vst3` covers 19 bundles
  and 236 effect classes in 24 s; LSP's single bundle of 184 classes takes
  19.9 s of that. Debian's `dataexchange.vst3` fails to load (undefined
  symbol `WaveformViewManager::createNanoVGViewAndContext`). It costs one
  "unusable" entry. The mda instruments are listed as unusable: they have no
  audio input, and this host chains effects.
- **Round trip, 20 000 blocks, measurement wait.**

  | Chain | p50 | p99 | p99.9 | max |
  |---|---|---|---|---|
  | AGain | 62–69 µs | 201–283 µs | 360–512 µs | 431–1730 µs |
  | dpl → LSP → AGain | 70 µs | 188 µs | 295 µs | 513 µs |
  | eg-amp (LV2, reference) | 59 µs | 178 µs | 227 µs | 410 µs |

  One AGain run had a single 2.47 ms host block and 21 drops. Two reruns had
  worst blocks of 108 and 104 µs and no drops. Recorded as a single outlier
  in the VM, not as a property of the plugin.

## Findings

1. **`getLatencySamples()` at activation is not the latency.** LSP returns 0
   after `setActive(true)` and 240 once it has processed. The first run
   reported 128 and measured 368: a 240-sample PDC error, caught by the null
   test. The host now re-reads latency after validation (4 blocks of
   silence), and `--list` runs 4 silent blocks before reading it.
2. **Runtime latency changes need `IMessage`.** LSP's processor sends its
   new latency to its controller as an `IMessage`. It creates that message
   through `IHostApplication::createInstance` from its own sync thread, and
   the controller then calls `restartComponent(kLatencyChanged)`. A host
   that does not create messages never hears of the change. The host
   creates `IMessage` and `IAttributeList`; it never does so on its
   processing thread.
3. **The report lags the change.** 13 blocks (35 ms) in this run, set by
   LSP's sync thread. PDC is wrong by the difference for that long. The
   contract's TODO(accept) on `latency_generation` re-planning covers the
   engine side. Nothing in the host can make the plugin report sooner.
4. **`getLatencySamples()` is a main-thread call.** The serving host runs
   process() on a thread of its own and keeps the thread that created the
   plugins as their main thread. `restartComponent(kLatencyChanged)`, from
   any thread, only sets a flag and wakes the main thread, which reads the
   latency and stores it in an atomic. The processing thread publishes it
   with the next block. An earlier version read it on the processing
   thread, which VST3 does not allow; the in-tree latency plugin segfaults
   if asked there (ctest `plugin_host_vst3_latency_change_main_thread`).
   The processing thread has a 1 MiB stack. `mlockall` populates and locks
   a new thread's whole stack, and with the default 8 MiB a crashing
   host's core dump delayed its exit. In CI the death was seen 55 (LV2) and
   86 (VST3) blocks after the trigger, against a 50-block test. In the
   container it was 17 to 18 blocks with 8 MiB and 9 to 14 with 1 MiB.
   Still a deviation: the VST3 workflow wants deactivate, read, reactivate,
   and the host reads without stopping. The conforming path is the
   contract's restart: a new host, then Ready, then a latency re-read.
5. **`setBusArrangements`' answer is not trusted.** The arrangement is read
   back with `getBusArrangement`, and a "yes" that kept stereo is refused.
   Buffers are laid out from that read-back, never from a later answer, and
   a later answer that differs is refused.
6. **"Declares 2, writes 8" looks different in VST3.** The plugin indexes
   past the channel-pointer array rather than past a buffer. Each bus's
   array carries 8 extra pointers into a canary-filled trap. The trap is
   checked after every `process` (64 words) and refused like an LV2
   overrun.
7. **One block carries as many parameter changes as the ring holds.** The
   host drains at most 256 ring entries per block into a change list of 256
   ParamIDs; the rest wait in the ring. It once held 128 and dropped the
   rest without a word. A change the plugin cannot take (unknown ParamID,
   non-finite value) is counted in the header's `params_rejected`.
8. **Parameters are normalised.** `ParamChange::value` is a VST3 normalised
   value but an LV2 port value. Recorded as a TODO(accept) in the contract:
   format-neutral values need a plain-to-normalised conversion, which is a
   controller call and must run off the processing thread.
9. **Resolving a class loads only bundles that can have it.** A bundle
   whose `moduleinfo.json` (VST 3.7.5+) names the class loads first. Bundles
   without a `moduleinfo.json` load next. A bundle whose `moduleinfo.json`
   does not name the class is never loaded. AGain's and LSP's bundles carry
   one. A bundle without one is loaded into the serving host itself, so its
   static constructors run there (BACKLOG Track 4).
10. **A scan runs plugin code, so each bundle is scanned in a child.** The
    child has a 120 s limit (`--scan-timeout`); 20 s would have killed LSP's
    19.9 s scan. A segfaulting factory costs one entry (ctest
    `plugin_host_vst3_list_crash_entry`, `..._after_crash`). The child's
    stdout goes to stderr, so plugin chatter cannot corrupt the list (AGain
    prints on every `setActive`). Each class is written and flushed as soon
    as it is described, and the parent keeps whole lines only, so a class
    that hangs costs itself and the rest of its bundle, and a kill in the
    middle of a record leaves no fragment for the failure record to join.
    The wait for the child's exit has the same deadline as its output (ctest
    `plugin_host_vst3_list_hang`: a 30 KB record, then a hang).

## Open

- Instruments: no event input and no audio input. Needed by
  `midi.instrument_tracks`.
- Sidechains: aux buses stay inactive and are fed silence.
- State: `IComponent::getState/setState`, controller state, and presets
  are not implemented.
- 64-bit processing and `kOffline` mode are not offered. Freeze and
  offline processing will want `kOffline`.
- Host conformance: `hostchecker.vst3` (in `vst3sdk-plugins`) has not been
  run against this host.
- Editors are out of scope (ADR-0010). Hosting them needs `IPlugView` and
  the Linux `IRunLoop`, which LSP's factory already looks for.
- Windows VST through yabridge is the next step (BACKLOG Track 4 item 6).
