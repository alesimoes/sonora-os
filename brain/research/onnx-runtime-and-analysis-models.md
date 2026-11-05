# ONNX Runtime and pretrained analysis models (native.ai_analysis, ADR-0011 N6)

Read 2026-11-04. Extends `plugin-licences-onnx.md` (#209). Every claim is
marked **[spec]** (the project's own documentation), **[impl]** (what a
third-party artefact does or claims) or **[measured]** (run here, 2026-11-04,
Docker `debian:trixie` on the Windows dev laptop; no audio was run, so no
accuracy or timing was measured).

## 1. ONNX Runtime

### 1.1 Debian trixie

- **[measured]** Packaged, `1.21.0+dfsg-1`: `libonnxruntime1.21`,
  `libonnxruntime-dev` (headers, `onnxruntime_cxx_api.h`, ORT_API_VERSION 21).
  `apt-get install --no-install-recommends libonnxruntime-dev` pulls 11
  packages. Installed sizes from `apt-cache show` (kB): libdnnl3.6 43,166;
  libonnxruntime1.21 16,523; libonnx1t64 4,591; libprotobuf32t64 3,520;
  libabsl20240722 2,561; libxnnpack0.20241108 2,105; libonnxruntime-dev 559;
  libre2-11 462; libgomp1 368; libpthreadpool0 126; libcpuinfo0 73.
  **Total 74,054 kB (about 72 MiB)** on amd64. `libdnnl3.6` is a hard
  dependency though only the oneDNN execution provider uses it; we cannot drop
  it without vendoring.
- **[measured]** A C++17 program using `Ort::GetVersionString()` compiled with
  `g++ -std=c++17 -Wall -Wextra ... -lonnxruntime` and printed `1.21.0`. The
  C++ API is a header-only wrapper over the C ABI (`onnxruntime_cxx_api.h`).
- **[impl]** Licence MIT (`plugin-licences-onnx.md`). **Closed here:** the
  LGPL scan. `ThirdPartyNotices.txt` (microsoft/onnxruntime `main`, 333,694
  bytes, read 2026-11-04) mentions the LGPL only in a reverse-engineering
  waiver (line 17), the MPL "Secondary License" definition (line 420) and
  Mbed TLS, which it uses under Apache-2.0, not its GPL alternative (line
  5503). `grep -i 'non-commercial'` finds only public-domain/permissive
  wording. No GPL/LGPL component is linked. Caveat: `main`, not the 1.21 tag;
  Debian's own `debian/copyright` is the authority for the packaged build.
- Vendoring is not needed on Linux. Not determined: what the default CPU
  provider's thread pool does at `Run()` (ADR-0011 Unverified 5): analysis in
  `sonora-analyze` runs off the audio thread, so that question does not block
  N6; it still blocks any per-block inference in the plugin path (ADR-0011
  4.2).
- Opset: ORT 1.21 sits on `libonnx1t64` 1.17. The exports below use opsets
  15 to 17 **[impl]**. Not determined: loading each model in 1.21 (do this in
  the implementation PR as a golden test).

### 1.2 Windows / MSVC (ADR-0008)

- **[impl]** Official release assets, v1.31.1 (GitHub API, 2026-11-04):
  `onnxruntime-win-x64-1.31.1.zip` 82.9 MB, `onnxruntime-win-arm64-1.31.1.zip`
  85.9 MB, also `onnxruntime-linux-x64-1.31.1.tgz` 11.4 MB and
  `-linux-aarch64` 10.3 MB. NuGet `Microsoft.ML.OnnxRuntime` exists; the docs
  state the Visual C++ 2019 runtime is required
  (https://onnxruntime.ai/docs/install/). The C API is ABI-stable, so the
  MSVC build links the import library from the zip.
- Not determined: the minimum MSVC toolset for the C++ wrapper headers; the
  Portable (MSVC) CI job has not built it. Debian's 1.21 and Microsoft's 1.31
  differ by ten releases; pin the API level with `OrtGetApiBase()->GetApi(21)`
  so both work.

### 1.3 Recommendation

Use Debian's `libonnxruntime-dev` 1.21 on Linux; link `onnxruntime` through a
CMake `find_library` guarded by an option, so Portable (MSVC) builds without
it (analysis passes report "model unavailable", falling back to the non-ML
path below). Windows consumes the Microsoft zip, fetched at build time and
pinned by sha256. Budget 72 MiB in the ISO.

## 2. Candidate models

Licence rule: weights must allow redistribution in a Debian-based distribution
and commercial use. Non-commercial or unclear is rejected.

### 2.1 Tempo and beat: Beat This! (CPJKU), ONNX export

- Source: https://github.com/CPJKU/beat_this ; paper arXiv 2407.21658 (ISMIR
  2024). **[spec]** "code and model weights MIT"; the README adds that some
  *training files* are fully copyrighted or CC-restricted and leaves that to
  the user. Upstream `LICENSE` (read): MIT, JKU Linz.
- Checkpoints (README): `final0..2` ~78 MB, `small0..2` ~8.1 MB, PyTorch
  Lightning. No official ONNX.
- ONNX **[impl]**, third party: https://huggingface.co/tuneperfect/beat-this-onnx
  (MIT; `final0`). Files via HF API, 2026-11-04:
  `beat_this.onnx` 81,935,641 B sha256
  `926847099887de0d577509824846c89797b02fb28e35fc004c3add149c465891`;
  `logmel.onnx` 4,469,774 B sha256
  `161bf43d3c258a3f5e98c609721dca6d259b8c75404a56c4c135ddd36dcdd8ad`. The
  card claims identical beats/downbeats to PyTorch on three songs only.
  Other exports exist (aaatmy, thebluescreen, ashudesai/songbird-models,
  musetric). Their opset/parity claims are self-reported.
- Input: mono 22,050 Hz; `logmel.onnx` audio[samples] -> spect[frames,128]
  at 50 frames/s; `beat_this.onnx` spect[1,frames,128], at most 1500 frames
  per chunk -> beat and downbeat logits[1,frames]. Chunking (1500 frames,
  6-frame borders) and peak picking (logit > 0, local max within +-3 frames,
  merge adjacent, snap downbeats to beats) are **not** in the graph; we
  implement them in C++ from `beat_this/inference.py`.
- Accuracy **[spec]** paper: GTZAN beat F1 89.1, downbeat F1 78.3, no DBN;
  weak continuity metrics (CMLt/AMLt) on complex pieces. Small model ~2M
  parameters "with strong scores" (numbers not read).
- Tempo is derived: median inter-beat interval of the beat list; there is no
  tempo head.
- Verdict: **recommended.** Open: reproduce the export ourselves
  (`scripts/ckpt2onnx.py` in danigb/beat-this-rs per docs.rs) so the bytes are
  ours; until then pin the tuneperfect hashes above. Our own parity gate is
  part of the implementation (beat positions within one frame vs the PyTorch
  reference on 3 CC0 clips).

### 2.2 Key and scale

| Candidate | Code | Weights | Verdict |
|---|---|---|---|
| Deezer S-KEY (arXiv 2501.12907) | MIT | **"only for non-commercial research purposes"** (README, quoted) | **Rejected.** Two HF ONNX exports (musetric/skey-onnx, aaatmy/skey-onnx) label it MIT "inherited from upstream": that is wrong against the upstream README. Do not use any S-KEY export. |
| Essentia key/tonal models | AGPL/BSD | CC BY-NC-SA on pretrained models (reported on the Essentia models page; not re-read here) | Rejected on that report; unverified here. |
| MusicalKeyCNN `keynet.pt` (a1ex90) | MIT | MIT repo `LICENSE` (Copyright 2025 Alexander Sommer); no separate weights clause | **Conditionally recommended**, see below |
| Krumhansl-Schmuckler on CQT chroma | n/a | no model | Fallback, always built |

MusicalKeyCNN (https://github.com/a1ex90/MusicalKeyCNN, fork Menschomat; the
OpenFadeR release carries an ONNX of it, hash below):
- `checkpoints/keynet.pt` 1,874,533 B, sha256
  `fc92072f1b9b19552ce3b74a9c8ce0cecb97633a12ae524ac0d93c05800e354e`
  (**[measured]** downloaded from `master` 2026-11-04; the commit SHA was not
  recorded: pin it when exporting).
- Input: mono 44,100 Hz, librosa CQT magnitude, `n_bins=105`,
  `bins_per_octave=24`, `fmin=65`, `hop_length=8820` (about 5 frames/s),
  shape (1, freq, time). Output 24 classes (Camelot index). `preprocess_audio`
  in `predict_keys.py`.
- Accuracy **[impl]** (README, GiantSteps key dataset): weighted MIREX 73.51,
  66.72 % exact.
- ONNX: none upstream. OpenFadeR
  (https://github.com/OpenRsG/OpenFadeR/releases/tag/models) ships
  `keynet.onnx` 1,852,740 B sha256
  `da9b9e947c9434069494302886f2e88e6b5cdd5e8c6acbe5eb97f693bf21fe17`, built
  from upstream commit `61d8fc1` with `examples/keynet_export.py`; its
  `keynet-LICENSE.txt` was not read. Prefer exporting ourselves.
- **Risks, stated plainly.** (a) Training data is the GiantSteps-MTG key
  dataset (Beatport previews). The dataset's licence was not found in its
  README; the weights' licence is the repo's MIT, but whether the derived
  weights are encumbered by the training audio is **not determined**. Same
  class of caveat as Beat This!'s. (b) A C++ CQT is needed (no librosa), which
  is real work, and frame-exact parity with librosa must be tested. (c) 66.7 %
  exact is not better than a classical chroma method on some material; the
  owner should read the figure as "a suggestion with a confidence".
- Verdict: **recommended with the training-data caveat recorded for the owner**;
  the classical Krumhansl pass ships regardless and is the default if the owner
  declines the caveat. The card is marked `status: conditional`.

### 2.3 Vocal and instrument presence: YAMNet

- Source: https://github.com/tensorflow/models/tree/master/research/audioset/yamnet ;
  licence Apache-2.0 per the TensorFlow Models repo root (a Google
  researcher's statement, https://groups.google.com/g/audioset-users/c/Ly4guQnlv_o,
  and the ONNX card). The YAMNet README itself states no licence. The class
  labels (AudioSet ontology) are CC BY 4.0 and need attribution.
- ONNX **[impl]**: https://huggingface.co/audiomagic/yamnet-onnx, tf2onnx
  opset 15, weights untouched. `yamnet.onnx` 16,093,355 B sha256
  `d3835ffbbd4a1bb3e777f0ca217b5007907f5171dd5d17c4236b95b2af8f908e`;
  `yamnet_class_map.csv` 14,096 B; `LICENSE` (Apache-2.0, Google 2022).
- Input: mono 16 kHz float [-1,1], dynamic length; log-mel front end inside
  the graph (25 ms window, 10 ms hop, 64 mel bins, 125-7500 Hz); one frame per
  0.96 s with 0.48 s hop. Outputs: scores[frames,521], embeddings[frames,1024],
  and log-mel patches. Needs >= 975 ms of audio.
- Accuracy **[spec]**: AudioSet eval balanced mAP 0.306, d-prime 2.318, lwlrap
  0.393 over 521 classes. These are tagging scores on YouTube clips, not on
  isolated stems; the numbers say nothing about a dry vocal stem.
- Useful classes (indices from the class map): 24 Singing, 25 Choir,
  132 Music, 135 Guitar, 136 Electric guitar, 137 Bass guitar, 138 Acoustic
  guitar, 148 Piano, 149 Electric piano, 153 Synthesizer, 157 Drum kit,
  158 Drum machine, 189 Double bass, 186 Violin. The gendered Singing classes
  were removed from AudioSet by Google, so we output `singing` only.
- Training data caveat: AudioSet (YouTube) licence not stated on the YAMNet
  page. Same class as above; Apache-2.0 on the weights is Google's statement.
- Not chosen: PANNs (arXiv 1912.10211): the page states no licence for the
  checkpoints and the repository was not read, so unclear, therefore rejected
  until read.
- Verdict: **recommended** for "is there singing / which instrument family".
  Presence is a threshold on the max of the per-frame scores; the threshold
  must be set on our own test clips, not taken from the mAP.

### 2.4 Loudness

Not ML. BS.1770 / EBU R128 in `common/dsp`, already N0.

## 3. Recommendation and manifest

| Pass | Model | Notes |
|---|---|---|
| Tempo / beats / downbeats | Beat This! `final0` ONNX (logmel + beat_this) | MIT; 86 MB; 22.05 kHz |
| Key and scale | MusicalKeyCNN `keynet` (conditional) over Krumhansl fallback | MIT, training-data caveat; 1.9 MB |
| Vocal / instrument presence | YAMNet | Apache-2.0 + CC BY 4.0 labels; 16 MB; 16 kHz |
| Loudness | BS.1770 DSP | no model |

`plugins/native/models/manifest.json` (proposed; array under `models`):
`name`, `version`, `file` (relative path after install), `url` (pinned
immutable: HF commit-hash URL or release asset), `sha256`, `size_bytes`,
`licence` (SPDX), `licence_url`, `weights_status` (`cleared` | `conditional`),
`training_data_note`, `card` (`brain/models/<name>.md`), `onnx_opset`,
`input` (`{sample_rate, channels, layout}`), `outputs`, `ort_min_api` (21),
`attribution` (text for the third-party notices). The loader refuses a file
whose sha256 differs and never downloads at run time (the image ships the
files; the ISO has no network guarantee). Model files are not committed to
git; the image build fetches them by manifest.

HF revision hashes for pinning (API `sha`, 2026-11-04):
tuneperfect/beat-this-onnx `93b53ffd41d7c69044b658a298b12278318c614d`;
audiomagic/yamnet-onnx `f25b741c2f0bdc6d7e6db24b5fddda23347dbafd`.

## 4. BACKLOG lines this closes

From `planning/BACKLOG.md`, "Licence follow-ups from
`plugin-licences-onnx.md` (#209)": it lists (a) LGPL scan of ThirdPartyNotices:
**closed** (2.1 above); (b) ONNX Runtime install size with dependencies:
**closed** (72 MiB measured); (c) MSVC and arm64 builds: **partly** (official
win-x64/arm64 assets exist; no MSVC build run, so leave open); (d) where
third-party notices ship, and (e) EBU 3341/3342 signals: **not touched**.
So the line stays open with (a) and (b) struck. The N6 line itself is not
closed. ADR-0011 Unverified 5 (allocation-free `Run()`) is still open and not
needed for off-thread analysis.

## 5. Not determined

1. GiantSteps-MTG and AudioSet training-data licence effect on the weights.
2. Beat This! `small` F1 figures; Beat This! parity of the third-party export
   beyond three songs.
3. Whether each model loads and matches its reference in ORT 1.21 (no audio
   was run).
4. Essentia model licences re-read from the source.
5. Real-time factor of Beat This! final0 on the target machine (86 MB model;
   a 4-minute song's cost is unmeasured).
