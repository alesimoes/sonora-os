# Model card: musicalkeycnn-keynet (key and mode)

Status: **conditional**. Ship only after the owner accepts the training-data
caveat; the non-ML Krumhansl-Schmuckler pass is the fallback and ships anyway.
Research: `brain/research/onnx-runtime-and-analysis-models.md` section 2.2.
Read 2026-11-04.

- Upstream: https://github.com/a1ex90/MusicalKeyCNN (method after Korzeniowski
  and Widmer), `checkpoints/keynet.pt`, 1,874,533 B, sha256
  `fc92072f1b9b19552ce3b74a9c8ce0cecb97633a12ae524ac0d93c05800e354e`
  (measured, `master` on 2026-11-04; record the commit when exporting).
- Licence: MIT (repo `LICENSE`, Copyright (c) 2025 Alexander Sommer); no
  separate weights clause. SPDX `MIT`.
- Training data: GiantSteps-MTG key dataset (Beatport previews). Its licence
  was not found, so whether the weights inherit a restriction is **not
  determined**. This is why the card is conditional.
- ONNX: none upstream. We export with `torch.onnx.export` from the pinned
  checkpoint (script in the implementation PR). A third-party export exists
  (OpenRsG/OpenFadeR release `models`, `keynet.onnx` 1,852,740 B, sha256
  `da9b9e947c9434069494302886f2e88e6b5cdd5e8c6acbe5eb97f693bf21fe17`, from
  commit `61d8fc1`; its licence file unread): not used.
- Input: mono 44,100 Hz; CQT magnitude, `n_bins=105`, `bins_per_octave=24`,
  `fmin=65`, `hop_length=8820` (about 5 frames/s); tensor (1,1,freq,time).
  Needs a C++ CQT matching librosa; test against librosa output.
- Output: 24 logits, Camelot index (12 major, 12 minor); argmax, softmax as
  confidence (uncalibrated).
- Reported accuracy (repo README, GiantSteps key dataset): weighted MIREX
  73.51, 66.72 % exact.
- REJECTED alternative: Deezer S-KEY weights, "only for non-commercial
  research purposes" (https://github.com/deezer/skey README). Its Hugging Face
  ONNX exports are labelled MIT in error.
- Not determined: training-data licence effect; frame-exact CQT parity;
  behaviour on short clips (< 30 s) and on non-Western tuning.
