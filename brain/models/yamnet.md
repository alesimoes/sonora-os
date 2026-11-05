# Model card: yamnet (vocal and instrument presence)

Status: cleared (Apache-2.0 weights, CC BY 4.0 labels, attribution required).
Research: `brain/research/onnx-runtime-and-analysis-models.md` section 2.3.
Read 2026-11-04.

- Upstream: https://github.com/tensorflow/models/tree/master/research/audioset/yamnet
  (MobileNet v1 depthwise-separable, about 3.7 M weights).
- Licence: Apache-2.0, per the TensorFlow Models repo and Dan Ellis in
  https://groups.google.com/g/audioset-users/c/Ly4guQnlv_o; the YAMNet README
  states none. Labels: AudioSet ontology, CC BY 4.0 (credit Google).
- Training data: AudioSet (YouTube); its audio licence is not stated on the
  YAMNet page. Owner to accept once.
- ONNX (third party, tf2onnx opset 15, weights untouched):
  https://huggingface.co/audiomagic/yamnet-onnx @ `f25b741c2f0bdc6d7e6db24b5fddda23347dbafd`
  - `yamnet.onnx` 16,093,355 B, sha256 `d3835ffbbd4a1bb3e777f0ca217b5007907f5171dd5d17c4236b95b2af8f908e`
  - `yamnet_class_map.csv` 14,096 B (521 classes); `LICENSE` (Apache-2.0).
- Input: `waveform` float32 [samples], mono 16 kHz in [-1,1]; log-mel inside
  the graph. At least 975 ms. One frame per 0.96 s, hop 0.48 s.
- Outputs: `output_0` scores [frames,521], `output_1` embeddings
  [frames,1024], `output_2` log-mel patches.
- Reported accuracy (YAMNet README, AudioSet eval, 521 classes): balanced mAP
  0.306, d-prime 2.318, lwlrap 0.393. Tagging on mixed clips, not stems.
- Classes we use: 24 Singing, 25 Choir, 132 Music, 135-138 guitars,
  148 Piano, 149 Electric piano, 153 Synthesizer, 157 Drum kit,
  158 Drum machine, 186 Violin, 189 Double bass. Gendered singing classes were
  removed upstream.
- Presence rule: max over frames of the class score against a threshold set
  on our own clips. No threshold is taken from the paper.
- Not determined: thresholds; behaviour on isolated or heavily processed
  stems; numeric parity of the ONNX export (the card gives behavioural checks
  only).
