# Model card: beat-this-final0 (tempo, beats, downbeats)

Status: cleared (training-data caveat below). Research:
`brain/research/onnx-runtime-and-analysis-models.md` section 2.1. Read 2026-11-04.

- Upstream: https://github.com/CPJKU/beat_this (Foscarin, Schluter, Widmer,
  ISMIR 2024, arXiv 2407.21658), checkpoint `final0`.
- Licence: MIT for code and weights (upstream `LICENSE`, JKU Linz). SPDX `MIT`.
- Training data: 18 datasets, 4,556 tracks; the README says some files are
  fully copyrighted or under limited CC licences and leaves the assessment to
  users. Owner to accept this once.
- ONNX files (third-party export, MIT):
  https://huggingface.co/tuneperfect/beat-this-onnx @ `93b53ffd41d7c69044b658a298b12278318c614d`
  - `logmel.onnx` 4,469,774 B, sha256 `161bf43d3c258a3f5e98c609721dca6d259b8c75404a56c4c135ddd36dcdd8ad`
  - `beat_this.onnx` 81,935,641 B, sha256 `926847099887de0d577509824846c89797b02fb28e35fc004c3add149c465891`
- Input: mono float, 22,050 Hz. `logmel`: audio[samples] -> spect[frames,128],
  50 frames/s. `beat_this`: spect[1,frames<=1500,128] -> beat, downbeat
  logits[1,frames].
- Post-processing is ours: 1500-frame chunks with 6-frame borders, peak pick
  (logit > 0 and local max within +-3 frames), merge adjacent peaks, snap
  downbeats to beats. Tempo = median inter-beat interval.
- Reported accuracy (paper): GTZAN beat F1 89.1, downbeat F1 78.3, no DBN.
  Weak on complex or under-represented material (continuity metrics).
- Parity of the export: claimed identical on three songs (self-reported).
  Our gate: beat times within one frame of the PyTorch reference.
- Attribution for third-party notices: "Beat This!, MIT, Copyright (c) 2024
  Institute of Computational Perception, JKU Linz".
- Not determined: real-time factor on target hardware; `small` variant
  (~8.1 MB) accuracy figures.
