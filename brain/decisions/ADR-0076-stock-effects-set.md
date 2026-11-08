# ADR-0076: The stock effects set: Debian's free LV2 packages, curated per package

Status: Proposed (recommended defaults, 2026-11-07; BACKLOG `plugins.stock`,
"the curated stock set").
Builds on: ADR-0004 (published and free work only), ADR-0011 §5 (code from
others) and "For the owner" item 4 (the stock set after the native suite),
ADR-0012 (out-of-process LV2 hosting, the probe), ADR-0002 and its
2026-11-02 amendment (one snapshot.debian.org timestamp for the archive).
Contract: `plugins/content/stock.json`, schema in `plugins/content/README.md`.
No protocol change, no engine change.

## Summary

The image ships a small set of free LV2 effects so a session has an EQ, a
compressor, a reverb and a delay before the native suite (ADR-0011) passes its
quality bar. The set is **Debian binary packages from the archive the image
is already built from**, installed into `/usr/lib/lv2`, which is on lilv's
default search path, so `sonora-plugin-host` finds them with no code change.
We vendor nothing and build nothing. Curation is per **package**, not per
plugin URI. `plugins/content/stock.json` lists one entry per package: name,
version pin policy, SPDX licence expression, categories, the native processor
that retires it and the condition for retiring it. Every licence in an entry
is on a GPL-3-compatible allow-list. The first manifest holds
`lsp-plugins-lv2`, `dragonfly-reverb-lv2`, `zam-plugins` and
`invada-studio-plugins-lv2`. `calf-plugins` is held out until its licence
reading is settled (below).

## Context

- The Complete DAW milestone needs effects in every insert slot now. The
  native suite (`plugins/native/*`) is being built stage by stage against
  ADR-0011 §3, and most processors have not passed that bar yet.
- The host already loads LV2 out of process (ADR-0012) and calls
  `lilv_world_load_all` (`plugins/host/src/lv2_host.cpp`). With `LV2_PATH`
  unset, lilv searches `~/.lv2:/usr/local/lib/lv2:/usr/lib/lv2`. Our native
  bundles go to `/usr/local/lib/lv2`. Debian's LV2 packages install into
  `/usr/lib/lv2`.
- The image is built from one snapshot.debian.org timestamp
  (`iso/auto/config`, `SONORA_SNAPSHOT`, trixie). The installed system's
  `sources.list` points at the live mirrors, so security fixes arrive through
  `apt`.
- ADR-0011 §5 lets only permissive third-party *code* into `common/dsp` and
  `plugins/native` until the owner rules on the suite's licence. That rule
  governs code we compile into our processors. Separately packaged
  programs that the host loads as plugins are a different case. They must
  still be redistributable beside a GPL-3.0-or-later product without a
  licence conflict, and they must contain nothing ADR-0004 forbids.
- ADR-0011, "For the owner" item 4, asks whether the stock set stays after
  the native suite meets §3. Each stock package therefore needs a named
  successor and a condition for retiring it.

## Decision

1. **Source.** The stock set is Debian binary packages from the archive
   pinned by `SONORA_SNAPSHOT`, installed by `live-build` from a package
   list. The OS packaging slice adds a commented block to
   `iso/config/package-lists/sonora.list.chroot` that names the packages in
   `stock.json`. Nothing is vendored into the repository, and nothing is
   compiled by us or installed into `/usr/local`.
2. **Location.** `/usr/lib/lv2`, which is lilv's default path. The engine,
   the host and the wire do not change. Every bundle goes through the same
   probe as any third-party plugin (ADR-0012). A bundle whose probe refuses
   it shows as unusable with its reason, and the package stays listed until
   a manifest change removes it.
3. **Curation granularity: the package.** A package is in the set or it is
   not. Every plugin a listed package installs appears in the scan,
   including ones outside v1 scope (for example `zam-plugins`' synths and
   `lsp-plugins-lv2`'s analysers and samplers). Hiding single URIs is
   deferred (see Alternatives rejected).
4. **The manifest.** The manifest is `plugins/content/stock.json` and has
   one entry per package:
   - `package`: the Debian binary package name.
   - `version_pin_policy`: `"snapshot"`. The version is whatever
     `SONORA_SNAPSHOT` carries. Bumping the timestamp moves the version.
     The installed system then follows the suite's security and point
     releases.
   - `spdx`: an SPDX expression that covers the code in the binary package,
     read from its `debian/copyright`.
   - `category`: one or more of `eq`, `dynamics`, `reverb`, `delay`,
     `modulation`, `saturation`.
   - `replaced_by`: the native processor (a `plugins/native/<name>`
     directory) whose arrival retires the package, or `null`.
   - `retire_when`: the condition, in words.

   The full schema and the check are in `plugins/content/README.md`.
5. **Licence allow-list.** These are the SPDX ids an entry's code may carry.
   All are GPL-3-compatible, so the set ships beside a GPL-3.0-or-later
   product (ADR-0004, ADR-0011 §5):
   - `GPL-3.0-or-later`, `GPL-2.0-or-later`
   - `LGPL-2.1-or-later`, `LGPL-3.0-or-later`
   - `MIT`, `ISC`, `BSD-2-Clause`, `BSD-3-Clause`

   Refused: `GPL-2.0-only`, which is incompatible with GPL-3, and any
   NonCommercial or no-derivatives licence. A package with any code file
   under a refused or unlisted licence stays out until the list is amended
   by an ADR amendment. Non-code assets are recorded in the README, not in
   `spdx`. Examples are artwork under CC-BY and fonts under OFL or
   Bitstream Vera. They must be redistributable, and none may be
   NonCommercial.
6. **No proprietary content.** The set contains no Apple or Avid content,
   names or presets. A package whose plugin names, presets or descriptions
   name a commercial product is refused under ADR-0004 and ADR-0011 §5.
7. **The first manifest.** The packages below exist in trixie
   (packages.debian.org, 2026-11-07). The licences come from their
   `debian/copyright` in stable.

   | Package | Version seen | Code licences | Categories | Retired by |
   |---|---|---|---|---|
   | `lsp-plugins-lv2` | 1.2.21-5 | LGPL-3+ (code), ISC and LGPL-2.1+ (vendored headers) | eq, dynamics, delay, reverb, modulation | `eq`, then `compressor`, `gate`, `limiter`, `multiband`, `delay`, `chorus`, `phaser` |
   | `dragonfly-reverb-lv2` | 3.2.10-3 | GPL-3+, GPL-2+ (freeverb), BSD-3 (kiss_fft) | reverb | `reverb` |
   | `zam-plugins` | 4.2+ds-1 | GPL-2+ | eq, dynamics, delay, reverb, saturation | `tube`, then `saturator`, `compressor`, `gate`, `eq`, `delay`, `reverb` |
   | `invada-studio-plugins-lv2` | 1.2.0+repack0-10 | GPL-2+ | eq, dynamics, delay, reverb, modulation, saturation | `delay`, then `compressor`, `phaser`, `tube`, `reverb`, `eq` |

   **`calf-plugins` is held out.** Its stable `debian/copyright` reads
   `LGPL-2+` for `Files: *`, with GPL-2+, ISC, MIT and public-domain files.
   `brain/research/open-data-and-dsp-sources.md` reads upstream as
   LGPL-2.1. LGPL-2.0-or-later is GPL-3-compatible, but it is not on the
   allow-list as written. Its plugins also duplicate what LSP and Zam
   already cover. The researcher settles the reading. Calf then enters by a
   one-line amendment, either to the list or to the manifest.
8. **Sunset (ADR-0011, "For the owner" item 4).** Each entry names its
   successor. A package is retired when every native processor in its
   `retire_when` passes ADR-0011 §3 and is installed by default. Until the
   owner rules otherwise, retiring is the recommended default: one quality
   bar, a smaller image, and fewer duplicate names in the browser. If the
   owner rules to keep the set, `retire_when` becomes informational, and the
   manifest stays as the licence record. A retirement is a manifest change
   plus the package-list change, in one PR. A saved session that references
   a retired plugin's URI loads with that insert unusable and its reason
   shown, as for any missing plugin.

## Rationale

- **Debian's packages, not our build.** Debian already builds these
  packages, checks their licences (`debian/copyright` is reviewed by
  ftpmasters), ships security updates and follows the suite we pin. Building
  them ourselves would add those duties without adding anything.
- **The package is the unit apt installs.** Curation at the package level
  matches what the image can actually control without an engine change. The
  manifest is small enough to review by eye.
- **`/usr/lib/lv2` is the path lilv already searches.** The feature needs no
  code, no protocol bump and no UI field. The probe already protects the
  session from a bundle that misbehaves.
- **Every entry names a successor.** The set stays a bridge to the native
  suite rather than becoming a permanent second suite, and the owner's
  item 4 has a concrete list to rule on.

## Consequences

- The browser lists every plugin in the four packages. That is about 200
  for LSP alone, some of them instruments and analysers outside v1 scope.
  This is accepted for now. Filtering is a follow-up.
- Stock and native plugins share categories, and some names are close (an
  "EQ" from LSP beside ours). Our native bundles stay distinguishable by
  their URI and vendor. Showing which plugins are stock is part of the
  filtering follow-up.
- The image grows by the packages and their dependencies (LSP pulls its own
  UI libraries). The size is unverified (below).
- The scan takes longer with several hundred more URIs. The probe cost is
  unverified (below). A slow scan is fixed in the scanner, not by dropping
  the probe.
- Package upgrades on the installed system may change a plugin's
  parameters or latency between sessions. These are upstream LV2 plugins
  with stable URIs, and the latency each insert reports is shown as for any
  plugin (`brain/specs/plugin-latency-compensation.md`).
- An ADR amendment is needed to add a licence to the list. A manifest PR is
  needed to add or retire a package.

## Alternatives rejected

- **Vendoring the sources and building them into `/usr/local`.** This adds
  build time to every image. Security fixes and licence review would become
  ours. It also duplicates what Debian already does for the same suite.
- **URI-level filtering in the scanner.** For example, an allow-list of
  plugin URIs from `stock.json`, with others hidden. This is an engine
  change and a wire field (`PluginInfo` would carry "stock"/"hidden"), plus
  a UI change, all for a cosmetic gain. It is deferred as a BACKLOG
  follow-up: "plugin browser: hide or group stock URIs outside v1 scope".
- **Shipping nothing until the native suite lands.** The DAW would have no
  EQ, compressor or reverb for months, and the Complete DAW milestone could
  not be tested on real mixes.
- **Installing every LV2 package in the archive.** This would pull in
  packages nobody has reviewed: synths, GPL-2.0-only code and large sample
  sets. It would also give no package a successor.

## Unverified

- **The image size delta.** The installed size of the four packages and
  their dependencies at `SONORA_SNAPSHOT`. Settled by
  `apt-get install --simulate` in the image chroot, and by the ISO size
  before and after.
- **Scan time with the set installed.** The wall time of a full scan and
  probe in `sonora-plugin-host`, cold and warm, on the target machine.
- **The probe result of each bundle.** Which URIs of each package pass
  `sonora-plugin-host`'s probe (declared against actual channels, crash,
  hang), and with what reported latency. A package whose effects mostly fail
  the probe leaves the manifest.
- **The licences.** The licences were read from `debian/copyright` in
  *stable* on 2026-11-07, through metadata.ftp-master.debian.org, not from
  the `SONORA_SNAPSHOT` (20261101) archive. The researcher confirms each
  package's `debian/copyright` and version at the snapshot. The researcher
  also confirms which non-code assets the binary packages actually install:
  - Zam's artwork, CC-BY (version not stated);
  - Dragonfly's fonts, OFL-1.1 and Bitstream Vera.
- **Calf's licence reading** (Decision 7).
- **Plugin names and presets.** That no plugin name, description or factory
  preset in the four packages names a commercial product (Decision 6).
