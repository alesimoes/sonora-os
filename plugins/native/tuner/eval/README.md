# Tuner evaluation on real voices (ADR-0077)

This directory holds the offline evaluation of the tuner on singing voices. The
`native_tuner_eval` tool reads a dataset directory the operator has downloaded
(vocadito for pitch accuracy, VocalSet for the processor's in-scale figure),
scores the YIN detector and the tuner processor, and writes a JSON report of
numbers. It never runs in the audio path, and no dataset is committed or fetched
by the build. Usage, the dataset download steps, the results table and the
open measurements are in `brain/research/native-tuner-eval.md`. The usage line is
`native_tuner_eval --data DIR --dataset vocadito|vocalset --out report.json
[--key N --scale NAME]`. No result is recorded in this repository until a
command has printed it.
