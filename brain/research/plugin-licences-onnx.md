# Plugin-suite licences (DPF, VST3 SDK, ONNX Runtime) and ONNX Runtime on trixie

Closes the BACKLOG item "Research, before N1" and ADR-0011 Unverified 1 and 3.
Checked 2026-11-01. Three labels are used: **licence text**, **implementations**
(what Debian ships), **measured here** (nothing; no Linux box was used).

## Summary

| Component | Licence | GPL-3.0-or-later compatible | Where settled |
|---|---|---|---|
| DPF at `4238e1c` (LV2, VST3 parts) | ISC | yes | below; already vendored |
| VST3 SDK 3.8 and later | MIT | yes | `brain/research/plugin-host-vst3.md` |
| VST3 SDK 3.7.x (Debian trixie) | Steinberg licence or GPL-3 (dual) | yes, under GPL-3 | same |
| ONNX Runtime 1.21 | MIT | yes | below |

## DPF (pinned `4238e1c7f0351bbe488d79f0899c540543ac7583`)

Source: the vendored copy, `plugins/native/format/third_party/dpf/` (`LICENSE`,
`LICENSING.md`, `SONORA.md`).

- `LICENSE`: ISC, "Copyright (C) 2012-2025 Filipe Coelho". The copyright and
  permission notice must appear in all copies.
- `LICENSING.md` per-format table: LV2 ISC (attribution to Steve Harris, David
  Robillard, and for the LADSPA-derived parts Furse, Barton-Davis,
  Westerfeld); VST3 ISC, "none, only DPF files used". DPF does not use
  Steinberg's SDK; its VST3 definitions are its own "travesty" headers under
  the same licence.
- Other targets in the table, not vendored by us, are not all ISC: LADSPA and
  DSSI headers are LGPL-2.1+ (upstream calls the commercial position unclear),
  VST2 is BSD-3 (Xaymar's clean-room vst2sdk), CLAP and JACK/standalone
  (RtAudio, RtMidi) are MIT. Do not vendor LADSPA/DSSI without a new decision.
- Obligation for us: ship DPF's `LICENSE` text with any binary containing DPF
  code. Where the image and releases put third-party notices is not decided.

## VST3 SDK

Recorded in `brain/research/plugin-host-vst3.md` (Steinberg press release
2025-10-29; `LICENSE.txt` in steinbergmedia/vst3sdk; Debian trixie
`libvst3sdk-dev` 3.7.10 dual GPL-3). Not repeated. Nothing new found.

## ONNX Runtime: licence

- Project `LICENSE` (microsoft/onnxruntime, main, read 2026-11-01): MIT,
  "Copyright (c) Microsoft Corporation".
  https://raw.githubusercontent.com/microsoft/onnxruntime/main/LICENSE
- `ThirdPartyNotices.txt` (same repo, main). The file is about 334k
  characters; only the first 100k were read. That part lists 19 components:
  protobuf, zlib, pybind11, Caffe, PyTorch, NumPy (BSD), ONNX, oneDNN,
  TensorFlow, DLPack, google/benchmark (Apache-2.0), Eigen (MPL-2.0), GSL,
  CNTK, HalideIR, HowardHinnant/date (MIT), LLVM (NCSA). No GPL, AGPL or
  noncommercial licence in what was read. The file's header mentions
  "libraries licensed under the GNU Lesser General Public License", so an
  LGPL component may sit in the unread remainder. **Not determined**; read it
  all before bundling an upstream build.
  https://raw.githubusercontent.com/microsoft/onnxruntime/main/ThirdPartyNotices.txt
- Debian's `debian/copyright` for 1.21.0+dfsg-1 (what trixie ships): mostly
  Expat; some Apache-2.0 and BSD-2/3 files; MPL-2.0 for `Barrier.h` and
  `EigenNonBlockingThreadPool.h`; public domain for MurmurHash3. Excluded from
  the source (Files-Excluded): `*.onnx`, `*.bin`, `*.ort`, `*.jar`, test
  media, because their licences are unclear. All compatible with GPL-3.
  https://sources.debian.org/data/main/o/onnxruntime/1.21.0%2Bdfsg-1/debian/copyright
- Models are separate: the MIT licence covers the runtime, not any `.onnx`
  file. Each model needs its own licence check (`native-plugin-suite.md`
  section 3).

## ONNX Runtime on Debian trixie (Unverified 3)

Source: packages.debian.org, trixie, read 2026-11-01. Implementation facts,
not measured.

- Packaged. All four packages are 1.21.0+dfsg-1: `libonnxruntime1.21`,
  `libonnxruntime-dev`, `onnxruntime-tools`, `python3-onnxruntime`.
- Architectures: amd64, arm64, armhf, ppc64el, riscv64, s390x.
- `libonnxruntime1.21` amd64: download 4,263,812 bytes (4.1 MB); installed
  size 16,523 kB (arm64 15,148 kB). Files: `libonnxruntime.so.1.21`,
  `libonnxruntime_providers_dnnl.so`, `libonnxruntime_providers_shared.so`.
- `libonnxruntime-dev` amd64: download 83.3 kB (installed size not read).
- Depends: libc6 >= 2.38, libstdc++6 >= 14, libgcc-s1, `libonnx1t64` >= 1.17,
  `libprotobuf32t64`, `libre2-11-absl20240722`; on amd64, arm64 and armhf
  also `libcpuinfo0`, `libpthreadpool0`, `libxnnpack0.20241108`; `libdnnl3.6`
  except armhf and riscv64.
- The runtime costs the image 16.5 MB plus its dependencies. The dependencies'
  sizes (protobuf, onnx, dnnl, xnnpack, re2) were **not read**; the real
  figure needs `apt-get install -s libonnxruntime1.21` in a trixie container.
- No vendoring needed on Linux, so the "build cost of vendoring" branch of
  Unverified 3 does not arise.
- Windows (ADR-0008): the install page names the CPU package
  `Microsoft.ML.OnnxRuntime` (NuGet) and says "Windows builds require Visual
  C++ 2019 runtime". https://onnxruntime.ai/docs/install/ . **Not
  determined**: supported MSVC versions, arm64, and whether an MSVC import
  library ships per architecture.

## Open questions

1. Read all of ONNX `ThirdPartyNotices.txt` (last ~234k characters) for LGPL.
2. Total installed size with dependencies (container, `apt-get -s`).
3. ONNX Runtime on Windows/MSVC: versions, arm64.
4. EBU Tech 3341/3342 test-signal redistribution (ADR-0011 Unverified 1,
   fourth bullet) is a separate question and was not looked at.
5. Whether 1.21.0 supports the ONNX opsets of the models we will train.
