# Tuner evaluation on real voices: how to run it, and what is still to measure

Companion to ADR-0077 (`brain/decisions/ADR-0077-tuner-real-voice-evaluation.md`).
The datasets are never committed or fetched by the build; the operator downloads
them to a local directory and runs the offline tool `native_tuner_eval`.

**No number in this note has been measured.** The results table is empty on
purpose. A row is filled only from the output of a command, with the machine and
settings written next to it.

## Datasets

| Dataset | Zenodo record | Version (as checked 2026-11-08) | Licence | Archive | Used for |
|---|---|---|---|---|---|
| vocadito | **5578259** | v2, published 2021-10-11 (a newer version exists; check it) | CC BY 4.0 | `vocadito.zip`, 58.5 MB | Raw pitch accuracy (RPA, RCA, voicing) against frame f0 |
| VocalSet | **1442513** | v1.2, published 2018-03-08 | CC BY 4.0 | `VocalSet1-2.zip` 6.0 GB, `VocalSet11.zip` 2.1 GB | Latency and correction quality (no f0 labels) |

**Record check for vocadito.** The record requested as 4714356 is not vocadito.
Zenodo 4714356 is a taxonomic figure of beetles (Liu, Lan-Yu, 2021, CC BY 4.0).
The vocadito record is **5578259** (Bittner, Pasalo, Bosch, Meseguer Brocal,
Rubinstein; "vocadito: A dataset of solo vocals with f0, note, and lyric
annotations"). The research note `open-data-and-dsp-sources.md` already cites
5578259. This note uses 5578259. The researcher should confirm it before anyone
cites a number.

## How to fetch

Download by hand, to a directory outside the repository. Zenodo serves each file
at `https://zenodo.org/records/<id>/files/<name>?download=1`.

```sh
mkdir -p ~/data/vocadito && cd ~/data/vocadito
curl -L -o vocadito.zip "https://zenodo.org/records/5578259/files/vocadito.zip?download=1"
unzip vocadito.zip

mkdir -p ~/data/vocalset && cd ~/data/vocalset
curl -L -o VocalSet1-2.zip "https://zenodo.org/records/1442513/files/VocalSet1-2.zip?download=1"
unzip VocalSet1-2.zip
```

Unverified: the download URL pattern and the archive's inner layout. The tool
expects `<name>.wav` files with a matching `<name>_f0.csv` (for vocadito)
somewhere under `--data`, and reads each CSV as `time,hz` (comma, semicolon or
tab separated, one frame per line, unvoiced frames at 0 Hz or negative). The
real file names and CSV columns in the current vocadito archive are not checked
yet (ADR-0077 Unverified). Check them before the first run. If they differ, fix
the reader; do not rename the data.

## The command

The tool has no `--help` flag. It prints this usage line when `--out` or
`--dataset` is missing (`plugins/native/tuner/eval/app/main.cpp`):

```
usage: native_tuner_eval --data DIR --dataset vocadito|vocalset --out report.json [--key N --scale NAME]
```

- `--data DIR`: the dataset directory (searched recursively).
- `--dataset vocadito|vocalset`: vocadito is scored against f0; vocalset has no labels, so only the processor figures apply.
- `--out report.json`: JSON of numbers only, with the dataset's licence and version fields.
- `--key N`: tonic, 0 to 11 (0 = C). Default 0.
- `--scale NAME`: `chromatic`, `major`, `minor`, `harmonic-minor`, `major-pentatonic` or `minor-pentatonic`; an index 0 to 5 also works. Default `major`.

Exit codes: 0 ok; 1 failure; 2 the data is missing (nothing is written).

Build the target `native_tuner_eval` (in `plugins/native/tuner/CMakeLists.txt`,
guarded by `sonora_audiofile`) with `tools/build-slot.sh`, then run it from the
build directory. The binary's path depends on the build layout and is not
confirmed here.

```sh
native_tuner_eval --data ~/data/vocadito --dataset vocadito --out vocadito-report.json --key 0 --scale major
native_tuner_eval --data ~/data/vocalset --dataset vocalset --out vocalset-report.json --key 0 --scale major
```

## Results (first measured run, 2026-11-13)

Machine: Intel i7-11800H, Linux container `sonora-build:trixie` (GCC 14.2,
Release), under Windows 11 Docker Desktop. Commit `a5bd87ac` (tool unchanged).
Settings: tuner key 0, scale `major` (index 1), YIN. Reports (numbers only) were
written with `--out`; each cell is the tool's printed value, rounded to 3 places.

Datasets: vocadito record 5578259 (version 2 on its card), all 40 clips, the
tool's loader unchanged (its CSVs are `time_s,f0_hz` at a 256/44100 s hop, 0 Hz
unvoiced; this matched the loader). VocalSet: record 1442513, file
`VocalSet11.zip` (the 2.1 GB archive; the card's v1.2 file was not used), the
101 clips under `*/scales/straight/` only (a subset, not the whole set).

```sh
native_tuner_eval --data D/vocadito --dataset vocadito --out vocadito-report.json
native_tuner_eval --data D/vocalset --dataset vocalset --out vocalset-report.json --key 0 --scale major
```

| Dataset | Measurement | Detector range | Value |
|---|---|---|---|
| vocadito | RPA, 50 cents (YIN) | low / medium / high | 0.922 / 0.917 / 0.716 |
| vocadito | RCA (YIN) | low / medium / high | 0.931 / 0.930 / 0.727 |
| vocadito | Voicing recall (YIN) | low / medium / high | 0.942 / 0.941 / 0.742 |
| vocadito | Voicing false alarm (YIN) | low / medium / high | 0.126 / 0.114 / 0.096 |
| vocadito | In-scale fraction, processor | | 0.806 |
| VocalSet scales/straight | In-scale fraction, processor | | 0.889 |
| vocadito | RPA, pYIN | | no published figure found (see below); not run |
| VocalSet | Latency, CPU class C | | not measured (the tool does not report them) |

The VocalSet in-scale figure is against C major while the singers' scales are
in their own keys; the processor snaps to the scale asked for, so this is how
well the corrected output sits on that scale, not a musical-key claim. vocadito
frames: 140782 per range.

pYIN: the vocadito paper (Bittner et al., arXiv 2110.05580) reports baselines
only for the Vocano note transcriber (Table 3) and CREPE (Table 4: RPA 0.98,
RCA 0.98, voicing recall 0.89, false alarm 0.31); it has no pYIN row. The
paper also says the f0 annotations were seeded by pYIN in Tony and then
corrected by hand, which would favour pYIN on this set. pYIN's RPA on
vocadito is therefore unverified.

## Open measurements

1. **YIN vs pYIN raw pitch accuracy on vocadito.** Run the tool for YIN (the
   shipped detector). For pYIN, first check the published RPA in the vocadito
   paper's baseline table. If the paper gives no figure, pYIN's number is a run
   of the reference implementation, recorded here with its source and version.
   ADR-0077 keeps YIN unless these numbers say otherwise.
2. **Latency and correction quality on VocalSet.** The tool's `--out` JSON
   reports `in_scale_fraction` (correction quality, from the scorer's independent
   estimator) but not latency. Latency must be measured elsewhere, and it must
   equal the latency the processor reports (PDC is sample-accurate or it is
   wrong). The tool removes the processor's reported latency before scoring, so
   the two figures are not the same measurement. The harness that reports
   latency is not confirmed here.
3. **CPU class C on the busiest vocal.** The tool does not report CPU. Pick the
   busiest VocalSet clip and measure it with the measure harness against the
   ADR-0011 §3.5 class C budget at the settings used above. A processor over its
   class ships gated off until it has an eco mode.
4. **The real-voice bars** are set from this run in `plugins/native/tuner/quality.json`
   (`real_voice`), ADR-0181.

## Licence and attribution

Both datasets are CC BY 4.0: use and redistribution are allowed with attribution.
The datasets are not committed to this repository or put in the image. Reports
name the dataset and its version; the tool writes `licence` and `version` fields
(today as a generic phrase, see Follow-ups).

Attribution text to use wherever a result is published:

> vocadito: Rachel Bittner, Katherine Pasalo, Juan José Bosch, Gabriel Meseguer
> Brocal, David Rubinstein. "vocadito: A dataset of solo vocals with f0, note, and
> lyric annotations." Zenodo, record 5578259, version 2. CC BY 4.0.
>
> VocalSet: Julia Wilkins, Prem Seetharaman, Alison Wahl, Bryan Pardo. "VocalSet:
> A Singing Voice Dataset." Zenodo, record 1442513, version 1.2. CC BY 4.0.

## Follow-ups

- Record the dataset version and licence from each dataset's card in the report, not as a generic phrase (tester note on `native.tuner.evaltool`).
- Check pYIN's published RPA on vocadito and the vocadito annotation layout (ADR-0077 Unverified).
