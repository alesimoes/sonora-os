# Open data, models and DSP code we can learn from or ship

Owner, 2026-10-13: "We'll need to search the internet for training data. We
have no way to train our own model [from our own recordings]. Try to find open
repositories with open-source DSP and plugins so we can understand and
replicate." The owner added more the same day:

- amp and speaker technical data, so we can train without recording the units
  (§D);
- microphone modelling (§E);
- an explicit survey of open neural-DSP repositories (§F);
- data for an AI vocal-balancing processor (§A.6).

This note extends `brain/research/native-plugin-suite.md` §3–§4. Where the two
disagree, this note is newer and checked against the primary source.

## How to read this note

- **Nothing here was measured.** No dataset was downloaded, no model was
  run, and no plugin was built. Every licence was read on 2026-10-13 from the
  URL given:
  - GitHub licences came from the `gh api repos/<owner>/<repo>` licence field
    and, where marked "(header)", from the source-file header read through the
    API;
  - dataset licences came from the Zenodo record's licence field or the
    dataset card.
- **Three kinds of claim are kept apart:**
  - **Spec**: what the licence, paper or datasheet says;
  - **Impl**: what the code or dataset actually contains;
  - **Measured here**: nothing yet.
- **Secondary** marks a claim that rests on a third-party summary, such as a
  compilation table, a retailer page or a search snippet. Confirm it before
  relying on it.
- **This is not legal advice.** It is an engineering reading. Counsel
  confirms anything the product depends on.

## Ground rules this note applies

1. **The product is GPL-3.0-or-later** (`LICENSE`).
   - Any licence with a **NonCommercial** term is incompatible (CC BY-NC,
     BY-NC-SA, BY-NC-ND, "research/educational only"). GPL-3 §10 forbids
     adding restrictions, and those terms are restrictions. This is the same
     reading as `native-plugin-suite.md` §3.
   - **GPL-2.0-only** code is incompatible with GPL-3. GPL-2.0-or-later,
     LGPL-2.1 and LGPL-3 are compatible (FSF licence list,
     https://www.gnu.org/licenses/license-list.html).
2. **ADR-0011 §5 (Proposed).** Until the owner rules on the suite's licence
   ("For the owner", item 1), only **permissive** third-party code enters
   `common/dsp` and `plugins/native`: MIT, BSD, ISC, zlib, Apache-2.0.
   - So the "Copy?" column below has three values:
     - **copy now**: permissive;
     - **copy after the ruling**: GPL- or LGPL-compatible, but copyleft;
     - **learn only**: incompatible or non-commercial.
   - "Learn only" means read the paper or the code to understand the method,
     then write our own implementation from the paper. Never transcribe it.
3. **ADR-0011 §4.6 data rules.**
   - Training data must be data we own, or data whose licence allows training
     and redistribution of the resulting weights.
   - "Unclear" means excluded.
   - Never allowed: scraped commercial recordings, and **audio rendered
     through a commercial plugin**.
   - Third-party captures and IRs need a written licence that allows
     redistribution.
4. **Weights inherit their data's problem** (`native-plugin-suite.md` §3).
   - A permissive code licence on a repository says nothing about the
     training data behind the released weights.
   - Each model below is flagged on both: the licence of the code and
     weights, and the licence of the training data.
5. **ADR-0004.** Trademarks, presets, IRs and samples of commercial products
   are never copied. Nothing here involves disassembly.

---

## A. Training data and pretrained models

### A.1 Guitar: DI, amp and effect data

| Dataset | Licence (as read) | Commercial use and shipping weights | Size and content | Use for us |
|---|---|---|---|---|
| **GuitarSet** (https://zenodo.org/records/3371780) | CC BY 4.0 (Zenodo licence field) | **Yes**, with attribution | 8.2 GB. Acoustic guitar, hexaphonic pickup plus mic, pitch, chord and beat annotations | Tuner and pitch evaluation; guitar DI-like input material for virtual captures (§D) |
| **EGFxSet** (https://zenodo.org/records/7044411) | CC BY 4.0 (Zenodo licence field) | **Yes**, with attribution. *But* see the note below | 5.8 GB. 8,970 clips of 5 s. Clean Stratocaster DI (690 clips, 5 pickup positions) and the same through 12 **hardware** effects: three drives, chorus, flanger, phaser, a delay unit in three modes, and a combo amp's reverb in three modes. Settings annotated | Test material for drive, modulation and delay models. The clean DI is reusable input for our own captures |
| **IDMT-SMT-Guitar** (https://zenodo.org/records/7544110) | **CC BY-NC-ND 4.0** (Zenodo licence field) | **No**. NonCommercial, and NoDerivatives | 1.3 GB. Playing techniques, single notes, chords | Excluded. Could only be read for its method |
| **EGDB** (https://ss12f32v.github.io/Guitar-Transcription/) | **No licence stated** on the project page; download is a Google Drive folder | **Excluded** (unclear) | 240 tablatures; DI plus renders labelled "Marshall", "Fender-twins", "Mesa" (page does not say whether hardware or plugins) | Excluded. Derivative sets made by re-rendering EGDB's DI through commercial plugins (e.g. arXiv 2608.05513 used Neural DSP plugins) are doubly excluded (ADR-0011 §4.6) |
| **SignalTrain LA-2A** (https://zenodo.org/records/3824876) | CC BY 4.0 | **Yes**, with attribution | 20.7 GB. Input and output of a hardware opto compressor at many Peak Reduction settings | Validation of the opto model (N2); training a micro-TCN-class model if a black-box path is ever allowed |
| **GuitarML ToneLibrary** (https://github.com/GuitarML/ToneLibrary) | GPL-3.0 (repo licence) on the JSON model files | GPL-3 permits redistribution. **But** models are contributed by email (README); no per-model record of the unit captured, or whether it was hardware or a plugin | Proteus, NeuralPi, SmartAmp and SmartAmpPro models (LSTM, WaveNet) | **Excluded** under ADR-0011 §4.6. A capture card needs the unit and its provenance, and a capture of a plugin is forbidden. Usable to test our loader |
| **TONE3000 (formerly ToneHunt) NAM captures** (https://www.tone3000.com/policy, tone pages) | "**T3K**" on the tone pages read (e.g. https://www.tone3000.com/tones/deltalab-tube-driver-28229): use in software and publish outputs royalty-free, but **do not upload, republish or distribute the file without the author's permission**. No CC option was seen on any page read | **No redistribution**. Users may load them themselves | Thousands of community NAM captures and IRs | **Excluded from the image.** A user may load their own downloads into our NAM loader (N3); that is their licence, not ours. Whether any uploader offers a CC licence is **open** |

What this section establishes:

- **No downloadable amp-capture corpus exists that we may ship.**
  - That matches ADR-0011's position: our captures come from hardware we own
    or rent, or (new here) from simulation (§D).
- **EGFxSet is the only CC BY guitar set with hardware processing.**
  - It is CC BY and fine to ship weights from.
  - Its effects are trademarked units. A model trained on it must not carry
    the unit's name (ADR-0011 §5).

### A.2 Cabinet impulse responses

- **No guitar-cabinet IR set with a verified redistribution licence was
  found.**
  - **Soundwoofer** (https://soundwoofer.com/Library/Index) is described as
    "available to the public domain" in a 2019 forum post by its makers
    (https://www.kvraudio.com/forum/viewtopic.php?t=529289). No licence text
    was found. **Secondary; unclear means excluded.**
  - A third-party page (https://darwinscat.com/sound-utils/cabinet-ir-utility,
    via search) describes a "Brutal Pack" by Jester Dyne Productions as CC0.
    No primary source was found. **Unverified.**
  - **Forward Audio faIR** packs are free, but licensed per account
    (https://www.forward-audio.com/downloads). **Not redistributable.**
  - Most free IR packs carry no licence at all. Treat them as all rights
    reserved (`native-plugin-suite.md` §1.11).
- **Conclusion: cabinet IRs are ours to make.** Three routes:
  - capture a cabinet we own or rent (ADR-0011 §4.6);
  - synthesise approximations from published speaker data (§D.2);
  - load the user's own IRs. They hold that licence, not us.

### A.3 Room and reverb impulse responses

| Dataset | Licence | Ship weights or IRs? | Content |
|---|---|---|---|
| **Arni** (https://zenodo.org/records/6985104) | CC BY 4.0 (Zenodo) | Yes, attribution | 50.9 GB. 132,037 RIRs from one variable-acoustics room at Aalto, 5,342 absorption configurations |
| **Motus** (https://zenodo.org/records/4923187) | CC BY 4.0 (Zenodo) | Yes | 63.6 GB. 3,320 higher-order Ambisonic RIRs; 830 furniture configurations with 3D models |
| **BUT ReverbDB** (https://speech.fit.vut.cz/software/but-speech-fit-reverb-database) | "CC-BY 4.0" (project page); `license_text` not read | Yes, for the RIR-only part. The retransmitted-speech part carries LibriSpeech, HUB5 and NIST terms | 9 rooms, ~4,700 RIRs, 8.7 GB RIR-only |
| **Aachen AIR** (https://www.iks.rwth-aachen.de/en/research/tools-downloads/databases/aachen-impulse-response-database/) | Page says "see the included MIT license file" (file not opened) | Probably yes. **Confirm in the archive** | 344 RIRs (secondary count) |
| **dEchorate**, **MeshRIR**, **FLAIR**, **MP-RIR** | CC BY 4.0 per the compilation at https://github.com/RoyJames/room-impulse-responses | **Secondary**; confirm each record | Assorted |
| **SRIRACHA** | CC BY-SA 4.0 (same compilation) | ShareAlike on the IRs. Fine for GPL-3 if the IRs ship under BY-SA terms. Weights: open | 2.6 M spatial RIRs |
| **OpenAIR** (https://www.openairlib.net/) | **Per item.** Uploaders choose any CC licence or all rights reserved (York page, via search) | Per IR: yes only for items marked CC BY, BY-SA or CC0 | Concert halls, churches, outdoor spaces |
| **MIT IR Survey** (https://mcdermottlab.mit.edu/Reverb/IR_Survey.html) | **No licence on the page** (read). A Hugging Face mirror claims CC BY 4.0 | **Excluded until the lab confirms** | 271 everyday-space IRs |
| **Voxengo free IRs** (https://www.voxengo.com/impulses) | Royalty-free use "for any purpose, including commercial", but **no profit, direct or indirect, from distributing the files** | Usable to *use* or *train*. **Not to ship** in a product we might sell | ~40 synthetic IRs made with a discontinued modeller |
| **EchoThief** (https://www.cs.bu.edu/fac/snyder/cs583/Homeworks/ImpulseResponses/EchoThiefImpulseResponseLibrary/EchoThief%20License.pdf, a copy hosted on a course page) | Derivative works allowed; any other use needs permission | **Excluded** for shipping | ~115 locations |
| **C4DM RIR**, **MIRACLE** | CC BY-NC-SA per the compilation https://github.com/RoyJames/room-impulse-responses (**secondary**, read 2026-10-13; not confirmed at the source) | **No** | — |
| **ACE Challenge** | CC BY-ND 4.0 per the compilation https://github.com/RoyJames/room-impulse-responses (**secondary**, read 2026-10-13; not confirmed at the source) | **No.** NoDerivatives | — |

**What we would use them for:**
- training and evaluating the blind RT60, DRR and pre-delay estimators
  behind the reverb suggestions (ADR-0011 §4.4, N5; `native-plugin-suite.md`
  §2.5);
- augmentation for every analysis model, which must work on reverberant
  input.

Reverb *presets* are our own FDN designs, not shipped third-party IRs.
Arni and Motus are the clean shippable sets if we ever ship a convolution
"real space".

**Synthetic RIRs are a fallback with no licence question.**
- **pyroomacoustics** (MIT; https://github.com/LCAV/pyroomacoustics):
  image-source plus ray tracing.
- **gpuRIR** is **AGPL-3.0** (https://github.com/DavidDiazGuerra/gpuRIR).
  The AGPL applies to the code, not to RIRs it generates. Using it offline is
  fine, but keep it out of the product.

### A.4 Music stems: separation and mix analysis

| Dataset | Licence (as read) | Usable? | Notes |
|---|---|---|---|
| **MUSDB18-HQ** (https://zenodo.org/records/3338373) | Zenodo: "Other (Non-Commercial)". Notes: "provided for educational purposes only … should not be used for any commercial purpose" | **No (confirmed)** | 22.7 GB. 150 tracks: 100 from DSD100 (Mixing Secrets library), 46 MedleyDB (CC BY-NC-SA 4.0), 2 Native Instruments, 2 CC BY-NC-SA 3.0 |
| **DSD100** (https://sigsep.github.io/datasets/dsd100.html) | No licence. Defers to the Cambridge-MT "Mixing Secrets" library | **No** | Cambridge-MT's terms: free "for educational purposes only", commercial use needs the rights holders' permission; research use not agreed with contributors (https://cambridge-mt.com/ms/mtk-faq, via search; the page itself returned 403 to the fetcher) |
| **MoisesDB** (https://github.com/moises-ai/moises-db) | **CC BY-NC-SA 4.0** (README "Licensing" and dataset-card header) | **No** | 240 tracks, 11 stem types |
| **MedleyDB** | CC BY-NC-SA (`native-plugin-suite.md` §3) | **No** | — |
| **MTG-Jamendo** (https://github.com/MTG/mtg-jamendo-dataset) | Metadata CC BY-NC-SA 4.0. Audio under per-track CC licences. Dataset "solely for non-commercial research and academic use"; other use needs Jamendo's written authorisation | **No** (as a dataset) | 55k tracks, 508 GB |
| **FMA** (https://github.com/mdeff/fma) | Code MIT, metadata CC BY 4.0. Audio "under the license chosen by the artist" | **Per track only.** A filtered subset of tracks whose own licence is CC BY, CC BY-SA or CC0 is usable. NC and ND tracks are not | 106,574 tracks. Mixes only, no stems: useful for mix analysis and reference statistics, not separation |
| **Slakh2100** (https://zenodo.org/records/4599666) | **CC BY 4.0** | Licence: yes. **ADR-0011 §4.6: open question** | 104 GB flac (~500 GB wav). 2,100 mixes plus stems, 145 h, rendered from Lakh MIDI "using professional-grade sample-based virtual instruments" (paper, https://arxiv.org/abs/1909.08494). The paper does not name the instruments; 187 patches, "many patches have effects built-in such as reverb, EQ, and compression". The Lakh MIDI files are scraped arrangements of mostly commercial songs. See note 1 |
| **StemGMD** (https://zenodo.org/records/7860223) | CC BY 4.0 | Licence: yes. **ADR-0011 §4.6: excluded as written** | 1.13 TB. Groove MIDI drum performances rendered per drum piece with **Logic Pro X sample libraries** (record text) |
| **E-GMD** (https://magenta.tensorflow.org/datasets/e-gmd; https://zenodo.org/record/4300943) | CC BY 4.0 (Google LLC) | Licence: yes. ADR-0011: **hardware** render, not a plugin, so the plugin rule does not apply as written | 444 h of drums, 43 kits re-recorded through a **Roland TD-17** module |
| **Cambridge-MT multitracks** (direct) | Educational only | **No** | — |
| **ccMixter** (https://ccmixter.org) | Per upload; many a cappellas are BY-NC (e.g. https://ccmixter.org/fortminor_samples) | **Per track.** CC BY uploads are usable, with attribution | Vocal stems and a cappellas (see A.6) |
| **Freesound** (https://freesound.org) | Per sound: CC0, CC BY or CC BY-NC; legacy Sampling+ should be treated as NC (forum, via search) | CC0 and CC BY only | Single sounds and loops; some multitrack material |

**Note 1: the rendered-through-commercial-instruments question.**
- ADR-0011 §4.6 bans "audio rendered through a commercial plugin". The
  stated reason is that training on it would be "black-box cloning by
  another route".
- Slakh, StemGMD and E-GMD are CC BY. Their audio is MIDI played through
  commercial sample instruments (Slakh, StemGMD) or a hardware module
  (E-GMD).
- A separation or transcription model trained on them does not learn to
  *reproduce* those instruments' processing. It learns to take them apart.
  That is different from the case the rule was written for (training an EQ
  or compressor to mimic a plugin).
- But the rule as written catches Slakh and StemGMD. It is the
  architect's and owner's call to narrow it.
- **Recommendation:** amend §4.6 to say "trained to imitate the output of a
  commercial processor". Until then, treat Slakh and StemGMD as excluded.
- Separately, Lakh MIDI's songs are arrangements of copyrighted
  compositions. The CC BY on Slakh covers the dataset, not the compositions.
  Counsel's question.

**No real, commercially licensed multitrack corpus of popular music exists
in this survey.** That is the biggest gap (§C.3).

### A.5 Vocals: pitch, tuner and key

| Dataset | Licence | Usable? | Content |
|---|---|---|---|
| **VocalSet** (https://zenodo.org/records/1442513) | CC BY 4.0 | **Yes** | 8.1 GB, 10.1 h. 20 professional singers, standard and extended techniques, all five vowels |
| **vocadito** (https://zenodo.org/records/5578259) | CC BY 4.0 | **Yes** | 58.5 MB. 40 short solo excerpts; frame f0, notes from two annotators, lyrics |
| **Dagstuhl ChoirSet** (https://zenodo.org/records/4607766) | CC BY 4.0 (v1.2) | **Yes** | 5.1 GB. Multitrack a cappella choir with close mics per singer. The f0 annotations described in the TISMIR paper (doi 10.5334/tismir.48) were not checked |
| **GuitarSet** | CC BY 4.0 | Yes | (A.1) Pitch of plucked strings |
| **MDB-stem-synth** (https://zenodo.org/records/1481172) | **CC BY-NC 4.0** (Zenodo licence field). `native-plugin-suite.md` called the CREPE data "MedleyDB-derived"; the licence is now confirmed | **No** | 230 resynthesised MedleyDB stems with exact f0 |
| **NUS-48E** | Original distribution by NUS on request (terms not found). A 2026 Zenodo re-upload (https://zenodo.org/records/19595152) says CC BY 4.0 but lists **no creator** | **Excluded.** The uploader's right to relicense is unestablished | 48 sung and spoken songs, 12 singers |
| **MIR-1K** | No licence found; the original host (mirlab.org) refused the connection | **Excluded (unclear)** | 1,000 Chinese karaoke clips |
| **GTSinger** | CC BY-NC-SA 4.0 (paper, https://arxiv.org/abs/2409.13832) | **No** | — |
| **pitch-benchmark** (https://huggingface.co/datasets/lars1234/pitch-benchmark) | CC BY-NC-SA 4.0 | **No** | SwiftF0's training mix (A.7) |

### A.6 Vocal balancing (owner item: harshness, sibilance, body, nasal mids, air)

**What the processor needs.**
- **Clean, dry, well-recorded vocals** across singers, styles and
  microphones. Mostly sung, with some spoken.
- **Labels for the five problem bands:**
  - harshness at 3–5 kHz;
  - sibilance at 5–10 kHz;
  - low-end body at 100–300 Hz;
  - nasal or "megaphone" mids at 800 Hz–2 kHz;
  - air above 10 kHz.

**No open dataset carries those labels.** The cheap way to get them is
**synthetic degradation**:
- take clean vocals;
- apply known, randomised EQ and dynamic deviations in each band (a boost at
  3.5 kHz, a sibilant shelf, a cut in body, and so on) with our own N0 EQ;
- train or fit the analyser to recover the deviation.

This is the standard self-supervised recipe for EQ and mix estimation (see
DeepAFx-ST and Diff-MST in `native-plugin-suite.md` §2; both are
non-commercial *code*, so learn from the papers only). The labels then cost
nothing and carry no licence. **Human judgement is still needed for
evaluation**: an expert panel rates "harsh" or "nasal" on held-out real
vocals. That panel is ours to run.

**Openly licensed vocal material usable commercially:**

| Source | Licence | Character | Fit |
|---|---|---|---|
| **VocalSet** | CC BY 4.0 | Studio singing, 20 trained singers, many techniques | Primary sung material; techniques such as belt and vocal fry exercise the harshness and nasal bands |
| **Dagstuhl ChoirSet** | CC BY 4.0 | Close-miked amateur choir, several mic types (paper) | Mic and singer variety. A cappella bleed is a realistic nuisance |
| **vocadito** | CC BY 4.0 | 40 solo excerpts, several languages | Evaluation |
| **VCTK 0.92** (https://datashare.ed.ac.uk/handle/10283/3443) | **CC BY 4.0** (the record's `license_text.txt` opens "Creative Commons License: Attribution 4.0 International") | Speech, 110 speakers, hemi-anechoic room. Recorded at 96 kHz/24-bit with DPA 4035 and Sennheiser MKH 800 mics; released at 48 kHz/16-bit | Clean full-band reference for "air" and sibilance; spoken, not sung |
| **Hi-Fi TTS** (https://openslr.org/109) | CC BY 4.0 (OpenSLR page, via search); source material is public-domain LibriVox | 292 h, 10 speakers, 44.1 kHz. Home-studio audiobook recordings, bandwidth ≥ 13 kHz and SNR ≥ 32 dB (paper, https://arxiv.org/abs/2104.01497) | *Real* uncontrolled mic and room colouration: realistic harshness and boxiness to detect |
| **Common Voice** | **Unverified for the audio.** The README of https://github.com/common-voice/common-voice (read 2026-10-13) puts the code under MPL-2.0 and the *sentence text* under CC0. It says nothing about the voice recordings, and neither did https://github.com/common-voice/cv-dataset. The official page https://commonvoice.mozilla.org/en/datasets was not readable to the fetcher. CC0 for the audio rests on unnamed search snippets only | Crowd speech on consumer mics, very varied quality | Robustness only; "megaphone" is common here. **Confirm the current terms**: the dataset's distribution and its terms have moved since 2019 |
| **ccMixter a cappellas** | Per upload; only CC BY or CC0 | Real produced pop vocals, often already processed | Realistic evaluation; filter by licence on each page |
| **Freesound** vocals | CC0 or CC BY only | Assorted | Small additions |
| **EARS** (https://github.com/facebookresearch/ears_dataset) | **CC BY-NC 4.0** ("CC-NC 4.0" in the README) | Anechoic 48 kHz speech, 100 h | **No** |

Recommended mix:
- training input: VocalSet, Dagstuhl ChoirSet and VCTK, plus synthetic
  degradations;
- robustness: Hi-Fi TTS;
- evaluation: vocadito and hand-picked CC BY ccMixter a cappellas;
- then our own sessions under release forms (A.8).

### A.7 Pretrained models: weights versus data

| Model | Code licence | Weights licence | Training data | Shippable? |
|---|---|---|---|---|
| **Spleeter** (https://github.com/deezer/spleeter) | MIT | **MIT.** The JOSS paper (doi 10.21105/joss.02154): "source code and pre-trained models … distributed under a MIT license" | **Deezer internal datasets**, not MUSDB: "we cannot release the training data for copyright reasons" (paper) | **Least-bad separation option.** The rights holder of the training catalogue released the weights under MIT. Residual risk: we rely on Deezer's representation. Quality is 2019-era (below Demucs) |
| **Demucs / HTDemucs** (https://github.com/adefossez/demucs; the facebookresearch repo is archived) | MIT | **No separate weights licence** in the README | `htdemucs`: "MusDB + 800 songs"; `mdx_extra` includes the MUSDB test set (README) | **No.** MUSDB18 is non-commercial, and the 800 songs are undocumented |
| **Open-Unmix** (https://github.com/sigsep/open-unmix-pytorch) | MIT (GitHub licence field, 2026-10-13) | `umxl`: "the weights are only licensed for non-commercial use (CC BY-NC-SA 4.0)" (https://github.com/sigsep/open-unmix-pytorch/blob/master/README.md, read 2026-10-13); `umxhq` and `umx`: trained on MUSDB18 (same README) | — | **No** |
| **BS-RoFormer / MDX23 community weights** (https://github.com/ZFTurbo/Music-Source-Separation-Training) | MIT (code) | Per checkpoint, mostly undocumented | MUSDB18 and private sets | **No** (unclear) |
| **CREPE / torchcrepe** (https://github.com/marl/crepe, https://github.com/maxrmorrison/torchcrepe) | MIT (GitHub licence field of both repos, 2026-10-13) | Bundled in the same MIT repos (no separate weights licence seen) | MDB-stem-synth (**CC BY-NC 4.0**, https://zenodo.org/records/1481172, read 2026-10-13) and RWC-synth (CREPE paper, https://arxiv.org/abs/1802.06182) | **No.** Retrain |
| **SwiftF0** (https://github.com/lars76/swift-f0) | MIT | MIT | pitch-benchmark train split (**CC BY-NC-SA**): MDB-stem-synth, NSynth, PTDB-TUG, MOCHA-TIMIT, CMU Arctic, with MUSDB18 augmentation | **No.** Retrain. The MIT training code (https://github.com/lars76/swift-f0-training) is a good recipe |
| **PESTO** (https://github.com/SonyCSLParis/pesto) | LGPL-3.0 | (repo) | Released model trained on **MIR-1K** (README) | **No** (MIR-1K unclear). The method is self-supervised: retrain on VocalSet, vocadito and GuitarSet |
| **RMVPE** (https://github.com/Dream-High/RMVPE) | Apache-2.0 | — | Not stated in the README | **Unclear: excluded** |
| **Basic Pitch** (https://github.com/spotify/basic-pitch) | Apache-2.0 | (repo) | GuitarSet, MAESTRO, Slakh, **iKala**, **MedleyDB** (paper Table 1, https://arxiv.org/abs/2203.09893) | **No.** MedleyDB is NC; iKala's terms were research-only and are not checked here |
| **beat_this** (https://github.com/CPJKU/beat_this) | MIT | **MIT**, but the README adds: "some of the training files are fully copyrighted or under limited Creative Commons licenses, and it is up to the user to assess" | 16 datasets | **Not without counsel.** The authors disclaim the data question explicitly |
| **madmom models** (https://github.com/CPJKU/madmom) | BSD-style (https://github.com/CPJKU/madmom/blob/main/LICENSE, "Source code files", read 2026-10-13) | **CC BY-NC-SA 4.0** (same `LICENSE`, "Data/model files") | — | **No** |
| **Essentia models** | AGPL (code) | NC (`native-plugin-suite.md` §3) | — | **No** |
| **micro-TCN** (https://github.com/csteinmetz1/micro-tcn) | Apache-2.0 | Weights on Google Drive with no separate licence | **SignalTrain LA-2A (CC BY 4.0)** | **Probably yes**, with attribution to SignalTrain. Confirm the authors intend Apache for the weights. Better: retrain ourselves (cheap) |
| **ChowCentaur / KlonCentaur** (https://github.com/jatinchowdhury18/KlonCentaur) | BSD-3 | BSD-3 (in repo) | ~4 min of the author's DI guitar processed through a **SPICE model** (README) | **Yes** (BSD-3). The exact precedent for §D's virtual capture |
| **NAM (core, plugin, trainer)** | MIT | No pretrained amp shipped as such | — | Code yes. Captures: A.1 |

**Key detection, tempo, mastering.**
- No permissively trained key or mastering *model* was found that we can
  ship.
- ADR-0011 N4 is rule-based (no model) and N6 needs a cleared one.
- **Key**: a template method needs no training data. Use Krumhansl–Kessler
  or Temperley profiles over a chroma computed with our own code; librosa
  (ISC) is the reference oracle.
- **Tempo**: an autocorrelation or comb-filter tempogram, also without
  training.
- **Mastering suggestions**: loudness and spectral rules. Matchering (GPL-3)
  is a reference-matching DSP to learn from.

### A.8 Building our own data cheaply (fallback, and the only clean data)

1. **Simulation-generated pairs (§D).**
   - Run DI through our own SPICE, WDF or nodal models of published circuits
     to get the target audio.
   - No licence question exists for the output.
   - KlonCentaur shows the pipeline. Mikkonen, Wright and Välimäki, "Sampling
     the user controls in neural modeling of audio devices", EURASIP JASMP
     2024:26 (doi 10.1186/s13636-024-00347-5; CC BY) use SPICE-generated
     data to study how densely to sample the controls.
2. **Self-rendered degradations with our own DSP.**
   - EQ, compression, reverb and saturation from our N0–N2 processors,
     applied to CC BY sources (VocalSet, GuitarSet, Arni IRs).
   - This gives labelled pairs for every "detect what is excessive" task.
     It is ours by construction; the inputs carry attribution only.
3. **MIDI rendered through free instruments we may redistribute.**
   - Use our own synth (N-stage), Surge XT (GPL-3), Dexed (GPL-3), or
     sample sets that are CC0 or CC BY (e.g. Freesound instrument sets after
     a licence check).
   - This is a Slakh-like separation corpus with no commercial-instrument
     question. The MIDI must be our own or CC-licensed (for example
     Groove MIDI, CC BY 4.0; https://magenta.tensorflow.org/datasets/groove).
     Not Lakh MIDI.
4. **Commissioned sessions with release forms** (ADR-0011 "For the owner",
   item 5).
   - Musicians record multitracks for us. The release grants:
     - perpetual, worldwide rights to use the recordings to train, evaluate
       and ship models, and to publish the recordings;
     - an assignment of, or licence to, the sound-recording copyright and
       the performers' rights.
   - The compositions must be the performers' own originals, or works in the
     public domain, so no publishing rights are owed.
   - A few days of studio time with local bands yields the commercially
     clean multitrack corpus that does not otherwise exist (§C.3).
   - Publishing it as CC BY would also help the field. That is the owner's
     call.
5. **CC0 stems from artists.**
   - Ask artists who already publish under CC (ccMixter, FMA) for stems
     under CC BY or CC0.
   - Record each grant per track in the dataset card.

---

## B. Open-source DSP and plugins

Licence from the GitHub API on 2026-10-13 unless marked otherwise. "Header"
means it was read from a source file.

| Project | URL | Licence (as read) | What it implements | Quality and maturity | GPL-3 compatible? | Copy? |
|---|---|---|---|---|---|---|
| **Airwindows** | https://github.com/airwindows/airwindows | MIT (`LICENSE`, © 2018 Chris Johnson) | Hundreds of small effects: console emulation, saturation, tape, dither, reverbs, EQ | Very active (pushed 2026-10-10). Idiosyncratic, mostly undocumented algorithms, but ear-tuned and cheap | Yes | **Copy now**, with notice |
| **x42 fil4.lv2** | https://github.com/x42/fil4.lv2 | GPL-2.0-or-later (header: "version 2, or (at your option) any later version") | Parametric EQ derived from Fons Adriaensen's work | Production quality, ships with Ardour/Mixbus users | Yes | After the ruling |
| **x42 dpl.lv2** (true-peak limiter) | https://github.com/x42/dpl.lv2 | GPL-3.0 | Look-ahead true-peak limiter | Production | Yes | After the ruling |
| **x42 darc.lv2** | https://github.com/x42/darc.lv2 | GPL-3.0 | Dynamic-range compressor | Production | Yes | After the ruling |
| **x42 meters.lv2** | https://github.com/x42/meters.lv2 | GPL-2.0 (API metadata; **headers not checked**) | EBU R128, K-meters, correlation, spectrum | Production | **Check headers**: if GPL-2.0-only, no | Learn only until checked |
| **x42 fat1.lv2** (autotune), **zconvo**, **convoLV2** | https://github.com/x42/fat1.lv2 etc. | GPL-2.0 (metadata; `native-plugin-suite.md` read fat1's header as or-later) | Pitch correction; convolution | Production | Yes if or-later | After the ruling |
| **x42-plugins** (umbrella) | https://github.com/x42/x42-plugins | No root licence (submodule collection) | — | — | Per sub-plugin | — |
| **LSP Plugins** | https://github.com/lsp-plugins/lsp-plugins | LGPL-3.0 | ~200 plugins: EQs, dynamics, multiband, convolution, impulse-response capture, loudness, analysers | Very active, high quality, careful engineering | Yes | After the ruling |
| **Calf Studio Gear** | https://github.com/calf-studio-gear/calf | LGPL-2.1 | Compressors, EQs, reverb, modulation, vintage delay, tube-ish saturators | Mature, less active | Yes | After the ruling |
| **Dragonfly Reverb** | https://github.com/michaelwillis/dragonfly-reverb | GPL-3.0 | Hall, room, plate and early reflections, built on Freeverb3 algorithms | Well regarded, active | Yes | After the ruling |
| **ZamAudio zam-plugins** | https://github.com/zamaudio/zam-plugins | GPL-2.0-or-later (ZamTube header) | ZamTube (WDF triode plus Yeh tone stacks), ZaMaximX2, ZamComp, ZamEQ, ZamGate | Mature, DPF-based | Yes | After the ruling |
| **DPF** | https://github.com/DISTRHO/DPF | ISC | LV2, VST3, CLAP, VST2 and JACK wrapper (ADR-0011 §1.2 adopts it) | Active | Yes | **Copy now** (dependency) |
| **DISTRHO-Ports** | https://github.com/DISTRHO/DISTRHO-Ports | No root licence; per port (JUCE ports of third-party plugins) | Assorted | Mixed | Per port | Check each |
| **Surge XT** | https://github.com/surge-synthesizer/surge | GPL-3.0 | Hybrid synth; its effects section (reverbs, delay, waveshapers, Airwindows ports, and Chowdhury tape and spring reverb) is the interesting part | Very active, excellent | Yes | After the ruling |
| **Vital** | https://github.com/mtytel/vital | GPL-3.0 code. README: do not use the "Vital" or "Tytel" names; **factory presets are under a separate, non-redistributable licence** | Wavetable synth | Last push 2023-05 | Yes (code only) | After the ruling; **never presets** |
| **Dexed** | https://github.com/asb2m10/dexed | GPL-3.0 | 6-operator FM (msfa engine) | Active | Yes | After the ruling |
| **Guitarix** | https://github.com/brummer10/guitarix | `trunk/COPYING`: own code GPL-2.0-or-later. Bundled zita-convolver and zita-resampler are GPL-3+, and `abgate` is LGPL-3+, "so that Guitarix as a whole must be conveyed under GPL version 3 or later" | Tube preamp and power-amp models (tables from circuit simulation), tone stacks, cabinet convolution, many effects | Mature, Faust-generated DSP | Yes | After the ruling |
| **Faust libraries** | https://github.com/grame-cncm/faustlibraries | **Per function**, from `declare … license` lines. `aanl.lib`: MIT (37 declarations). `compressors.lib`: GPL-3.0-only (24), STK-4.3 (6), MIT (1). `reverbs.lib`: GPL-3.0-only (2), MIT (3), STK (1). `tonestacks.lib`: LGPL-2.1-or-later. `tubes.lib`: Guitarix-derived tables, **no licence declaration in the file**. `stdfaust.lib`: LGPL-2.1+ with an exception for compiled output | Filters, anti-aliased nonlinearities (`aanl`), WDF (`wdmodels`), tone stacks, tubes, reverbs, compressors | Very active, well documented | Yes per function. Note: GPL-3.0-**only** functions fix our combined work at exactly GPL-3.0 | MIT and STK functions **copy now**; GPL and LGPL after the ruling; `tubes.lib` **unclear: use Guitarix directly** |
| **Ardour a-comp, a-eq, a-delay, a-reverb, a-exp** | https://github.com/Ardour/ardour/tree/master/libs/plugins | GPL-2.0-or-later (headers of a-comp.c, a-eq.c, a-reverb.c) | Basic compressor, EQ, delay, reverb | Simple, solid | Yes | After the ruling |
| **Zita (Fons Adriaensen)** | http://kokkinizita.linuxaudio.org/linuxaudio/downloads/index.html | zita-convolver, zita-resampler, zita-dpl1, zita-eq1: "GPL3"; zita-rev1, zita-at1: "GPL" (version not stated on the page) | Partitioned convolution, resampler, peak limiter, reverb, autotune | Reference quality | Yes | After the ruling; zita-convolver is a strong candidate for N3's cab convolution |
| **Rubber Band** | https://github.com/breakfastquay/rubberband | GPL-2.0-or-later (README), or commercial | Time-stretch and pitch-shift (R2 and R3 engines) | Industry-grade | Yes | After the ruling (library dependency) |
| **SoundTouch** | https://codeberg.org/soundtouch/soundtouch | LGPL-2.1 (commercial licence on request) | WSOLA time and pitch, BPM detection | Mature, active (2026-04) | Yes | After the ruling |
| **Signalsmith Stretch** | https://github.com/Signalsmith-Audio/signalsmith-stretch | MIT | Polyphonic pitch and time stretch | Active, good quality | Yes | **Copy now** |
| **aubio** | https://github.com/aubio/aubio | GPL-3.0 | Onset, pitch (YIN, YIN-FFT), tempo, MFCC | Mature | Yes | After the ruling |
| **Essentia** | https://github.com/MTG/essentia | **AGPL-3.0** | Huge MIR library | Mature | AGPL is GPL-3-combinable, but adds the network clause; its models are NC | **Learn only** |
| **librosa** | https://github.com/librosa/librosa | ISC | Python MIR: chroma, CQT, tempo, pYIN | Reference | Yes | **Copy now** (offline tools and test oracle) |
| **libebur128** | https://github.com/jiixyj/libebur128 | MIT | BS.1770 / R128 loudness | Reference | Yes | **Copy now**; test oracle for N0 metering |
| **pyloudnorm** | https://github.com/csteinmetz1/pyloudnorm | MIT | BS.1770 in Python | Reference | Yes | Test oracle |
| **Matchering** | https://github.com/sergree/matchering | GPL-3.0 | Reference-match mastering (EQ curve and loudness) | Active | Yes | After the ruling; learn its method for N4 |
| **ChowDSP: AnalogTapeModel (CHOW Tape)** | https://github.com/jatinchowdhury18/AnalogTapeModel | GPL-3.0 | Hysteresis tape model (Jiles–Atherton, solved with RK4 or NR), wow, flutter, loss filters | Excellent, documented in DAFx-19 | Yes | After the ruling. The paper is enough to reimplement |
| **ChowDSP: KlonCentaur** | https://github.com/jatinchowdhury18/KlonCentaur | BSD-3-Clause | WDF and nodal circuit model, plus an RNN trained on SPICE output | Reference | Yes | **Copy now** |
| **chowdsp_wdf** | https://github.com/Chowdhury-DSP/chowdsp_wdf | BSD-3-Clause | Header-only wave-digital-filter library, including R-type adapters | Active | Yes | **Copy now**; the backbone for N2 circuit models |
| **chowdsp_utils** | https://github.com/Chowdhury-DSP/chowdsp_utils | **Per module** (`LICENSE.md`). `chowdsp_core`, `chowdsp_math`, `chowdsp_simd`: BSD-3. `chowdsp_filters`, `chowdsp_eq`, `chowdsp_reverb`, `chowdsp_compressor`, `chowdsp_waveshapers`, `chowdsp_dsp_utils`: **GPLv3** (module headers) | Filters, EQ, ADAA waveshapers, reverbs | Active | Yes | BSD modules copy now; GPL modules after the ruling |
| **BYOD**, **ChowMultiTool**, **ChowPhaser** | https://github.com/Chowdhury-DSP/BYOD etc. | BYOD and ChowMultiTool GPL-3.0; ChowPhaser (jatinchowdhury18) BSD-3 | Guitar effects, many circuit models | Good | Yes | Per licence |
| **SwankyAmp** | https://github.com/resonantdsp/SwankyAmp | GPL-3.0 | Physically informed tube amp | Active (2026-10-13) | Yes | After the ruling |
| **pedalboard** | https://github.com/spotify/pedalboard | GPL-3.0 | Python effects host and rendering | Active | Yes | Offline tooling only |

---

## C. Recommended shortlist for ADR-0011 N0–N3

### C.1 Data and models

- **N0** (EQ, dynamics, delay, chorus, metering)
  - No training data needed.
  - Test oracles: **libebur128** and **pyloudnorm** (MIT) for BS.1770;
    **librosa** for spectra.
  - Test audio: **VocalSet**, **GuitarSet** and **Dagstuhl ChoirSet**
    (CC BY 4.0), so every golden file is redistributable.
- **N1**: none.
- **N2** (reverb, tape, tube, opto and FET, tuner DSP)
  - **SignalTrain LA-2A** (CC BY 4.0) to validate the opto model.
  - **Arni** and **Motus** (CC BY 4.0) to calibrate and evaluate the FDN
    reverb's decay and RT60 estimation.
  - **VocalSet** and **vocadito** (CC BY 4.0) to evaluate the tuner (YIN and
    pYIN accuracy, `native-plugin-suite.md` §6 item 4).
- **N3** (amp and cab)
  - **Virtual captures (§D) first.** SPICE or WDF models of published
    circuits generate the training pairs; the DI is **EGFxSet** clean
    (CC BY) and **GuitarSet**.
  - Then a few real captures of owned or rented units to validate (§D.3).
  - **Cab IRs**: our own, captured or synthesised (§D.2).
  - Code: **NeuralAmpModelerCore** (MIT) or **RTNeural** (BSD-3) for
    inference, and the **NAM trainer** (MIT) for training.
- **N5 and N6, early**
  - **Vocal balancing (A.6)**: VocalSet, Dagstuhl ChoirSet and VCTK
    (CC BY 4.0) plus synthetic degradations; evaluate on vocadito and CC BY
    a cappellas.
  - **Pitch**: retrain a SwiftF0- or PESTO-class model on CC BY data
    (VocalSet, vocadito, GuitarSet, Dagstuhl, and NSynth, CC BY 4.0 per
    https://magenta.tensorflow.org/datasets/nsynth, read 2026-10-13) with the MIT
    training code.
  - **Key**: template method, no model.

### C.2 Code to adopt first

Permissive, usable now:
- **DPF** (ISC);
- **chowdsp_wdf** (BSD-3);
- **RTNeural** (BSD-3);
- **NeuralAmpModelerCore** (MIT);
- **Signalsmith Stretch** (MIT);
- **libebur128** (MIT);
- **Airwindows** (MIT);
- Faust **`aanl.lib`** functions (MIT);
- the BSD modules of **chowdsp_utils**.

After the owner's licence ruling: **zita-convolver** (GPL-3), **LSP**
(LGPL-3), **Guitarix** tube tables (GPL-3+), **x42** dpl and fil4,
**Rubber Band**.

### C.3 Gaps: no shippable data exists

1. **Music source separation. This is the biggest gap.**
   - Every strong open model is trained on MUSDB18, MoisesDB or MedleyDB,
     or on undocumented data. All three named datasets are non-commercial.
   - Least-bad options, in order:
     - (a) **Spleeter**. Its weights are MIT by the rights holder's own
       statement, and trained on Deezer's catalogue. Quality is
       2019-level; good enough for reference analysis (N5's full-mix
       reference), not for user-facing stem export.
     - (b) **Our own model** trained on a commissioned multitrack corpus
       (A.8 item 4) plus Slakh-style renders through our own or GPL
       instruments (A.8 item 3), and Slakh itself if ADR-0011 §4.6 is
       amended (A.4 note 1).
     - (c) Avoid separation: ADR-0011 already says stem or solo references
       need none.
2. **Guitar-cabinet IRs** with a redistribution licence: none verified.
   Capture or synthesise.
3. **Amp captures** with a redistribution licence: none that meets the
   capture-card rule. Virtual capture (§D) plus owned or rented hardware.
4. **Pitch models** with clean weights: none. Retraining is cheap.
5. **Labelled mix and mastering judgements** ("this is harsh", "too much
   reverb"): none open. Synthetic degradations, plus our own expert panel.

---

## D. Amp and speaker technical data, and "virtual capture"

### D.1 Where published circuit information exists

**Manufacturer and archive schematics.** These are reference only: read
them, never redistribute them (D.4).

- **Fender.**
  - Fender has hosted amplifier schematics on its support site. A forum
    post points to "Amplifier Schematics | Fender Support" under Downloads;
    the old fender.com page survives in the Wayback Machine
    (https://music-electronics-forum.com/forum/schematic-requests/20059-links-to-fender-schematics).
  - The current official URL was **not reached** this session
    (`support.fender.com/...amplifier-schematics` returned 404).
  - Third-party mirrors: The Tube Store (https://www.thetubestore.com/fender-amp-schematics,
    including the '59 Bassman), Amp Archives, BMAmps.
- **Mesa/Boogie.**
  - Owner's manuals for current models are hosted by Gibson, the current
    owner (e.g. https://www.gibson.com/cdn/shop/files/fillmore50-WEB.pdf).
    They give control ranges and voicing descriptions, not schematics.
  - **US 4,211,893**, "Dual mode music instrument amplifier", R. C. Smith,
    filed 1978-11-13 (https://patents.google.com/patent/US4211893A, read
    2026-10-13). Google Patents lists it as "Expired - Lifetime", with
    anticipated expiration 1998-11-13. Google states that this status is an
    assumption, not a legal conclusion. **Needs review** (Open question 10).
    Also a 1980 JASA paper of the same title (JASA 68(5)).
  - **US 4,701,957**, "Dual mode music instrument preamplifier", R. C. Smith,
    filed 1986-11-26 (https://patents.google.com/patent/US4701957A, read
    2026-10-13). A preamp with separate clean and saturation-distortion
    modes. Google Patents lists it as expired, with anticipated expiration
    2006-11-26, under the same caveat. **Needs review** (Open question 10).
  - Patents are published technical disclosures, intended to be read.
- **Marshall, Orange.** No official schematic source was found this
  session. Marshall's early heads are widely documented as derived from the
  5F6-A (Kuehnel 2nd ed. adds a JTM45 chapter, below). **Open**: whether
  either maker publishes service data.

**Books and papers analysing the classic circuits.** These are facts and
analysis we may use.

- R. Kuehnel, *Circuit Analysis of a Legendary Tube Amplifier: The Fender
  Bassman 5F6-A* (ISBN 9780976982210, 2nd ed.). Preamp, tone stack,
  phase splitter and push-pull stage worked mathematically; the second
  edition adds the JTM45
  (https://www.abebooks.com/9780976982210/Circuit-Analysis-Legendary-Tube-Amplifier-0976982218/plp).
  Also *Guitar Amplifier Electronics: Fender Bassman*
  (https://www.tubesandmore.com/products/guitar-amplifier-electronics-fender-bassman).
- M. Blencowe, *Designing Tube Preamps for Guitar and Bass*, 2nd ed.
  (https://valvewizard.co.uk/Book1.html). Gain stages, tone controls,
  topology. The author's site also has free design articles.
- D. Yeh, J. O. Smith, "Discretization of the '59 Fender Bassman tone
  stack", DAFx-06 (`native-plugin-suite.md` §1.11). Also "Modelling of the
  Fender Bassman 5F6-A Tone Stack" (https://arxiv.org/abs/2110.02285).
- K. Werner et al., "The Fender Bassman 5F6-A Family of Preamplifier
  Circuits — A Wave Digital Filter Case Study", DAFx-16
  (https://www.dafx.de/paper-archive/2016/dafxpapers/37-DAFx-16_paper_53-PN.pdf).
  It uses the Dempwolf triode model at a 25-port R-type adapter.
- **Tube models.**
  - N. Koren, "Improved vacuum tube models for SPICE simulations", *Glass
    Audio* 8(5), 1996 (https://www.normankoren.com/Audio/Tubemodspice_article.html).
    The page gives the triode equation. The model files carry **no
    licence**; the page says only "copyright 2000–2012". Use the published
    **equation and fitted parameters** (facts), not the files.
  - K. Dempwolf, U. Zölzer, "A physically-motivated triode model for
    circuit simulations", DAFx-11, pp. 257–264
    (https://dafx.de/paper-archive/details/MGMeIyM6_9oAzOgKCuzEqw). Smooth,
    and models grid current. Dempwolf's dissertation:
    https://www.hsu-hh.de/ant/wp-content/uploads/sites/699/2019/10/Dempwolf_Diss_Druckversion.pdf.
- **Neural targets on simulated amps.**
  - E.-P. Damskägg et al., "Deep learning for tube amplifier emulation",
    ICASSP 2019 (https://arxiv.org/abs/1811.00334). A WaveNet variant is
    trained **on input and output of a SPICE model of a Bassman 56F-A**:
    the virtual-capture method, published.
  - L. Juvela et al., "End-to-End Amp Modeling: From Data to Controllable
    Guitar Amplifier Models" (https://arxiv.org/abs/2403.08559). An LSTM is
    compared to an offline SPICE model; listening tests found it matched a
    high-quality SPICE model.
  - Mikkonen, Wright, Välimäki 2024 (A.8): with SPICE-generated data, five
    sample points per control were found sufficient for the devices
    studied.
  - A. Wright et al., "Real-Time Guitar Amplifier Emulation with Deep
    Learning", Appl. Sci. 10(3):766, 2020 (doi 10.3390/app10030766). About
    3 minutes of audio is enough to train.

**Speaker data. Manufacturers publish T/S parameters and frequency
response.**

- **Jensen P12Q** (https://www.jensentone.com/vintage-alnico/p12q, read):
  - a full T/S table per impedance. 8 Ω version: Re 5.6 Ω, fs 90.5 Hz,
    Qms 11.57, Qes 3.33, Qts 2.03, Mms 26.8 g, Cms 115 µm/N, Bl 5.88,
    Vas 39.5 L, Xmax ±1 mm, Sd 490.9 cm², Le 0.67 mH;
  - plus an interactive frequency-response chart. **No impedance chart.**
  - Other Jensen models have the same page layout.
  - Third parties disagree with Jensen's own table: zZounds lists Qts 1.94
    and Vas 47.5 L for 8 Ω; loudspeakerdatabase.com lists the 16 Ω Qts as
    2.30 (https://loudspeakerdatabase.com/Jensen/P12Q). **Use the maker's
    figures, and expect ±10–20 % unit spread.**
- **Celestion:**
  - datasheets carry a frequency-response graph (the Vintage 30 sheet,
    https://c3.zzounds.com/media/Vintage30-70c480f5fde7003e751aeabddc6b3ca9.pdf).
    Retail listings repeat fs ≈ 70–75 Hz, Re 7.3 Ω (8 Ω), 100 dB sensitivity
    and 70 Hz–5 kHz (e.g. https://tubetown.net/ttstore/de/celestion-vintage-30-12-60-w-8-ohm.html);
    the sources disagree on fs;
  - the **official product page was not reached** (404), so whether
    Celestion publishes full T/S for guitar drivers is **open**.
- **Eminence and WGS** are known to publish T/S for guitar drivers. **Not
  checked this session.**
- **Cabinet dimensions** come from manufacturers' spec sheets (external
  W×H×D, open or closed back), and loudspeakerdatabase.com has a box
  plotter. Port and baffle details are rarely published: measure, or
  assume.

### D.2 The virtual-capture pipeline

The goal is training pairs without recording the unit.

1. **Netlist from published facts.**
   - Transcribe topology and component values from the schematic into our
     own netlist (ngspice, already planned for N2) or into a WDF or nodal
     model (chowdsp_wdf, BSD-3; ACME.jl, MIT).
   - Tubes: Koren or Dempwolf equations, with published fits for 12AX7,
     EL34, 6L6 and EL84.
   - Output transformer: start linear (leakage inductance, primary
     inductance, winding capacitance), then add a Jiles–Atherton or
     Preisach core ("For the owner": published transformer data is scarce;
     see D.3).
   - Power supply sag: model the rectifier and reservoir impedance. It
     matters for "feel".
   - Speaker load: the **impedance curve**, not a resistor. Fit Re, Le,
     fs, Qms and Qes to a two-port electrical equivalent so the power amp
     sees the real, frequency-dependent load. That is the
     power-amp/speaker interaction, the reason an amp into a resistor
     sounds wrong.
2. **Drive it with DI.**
   - Clean guitar DI: EGFxSet and GuitarSet (CC BY), then our own.
   - Also the standard sweep, noise and level-step sequences used by NAM
     and Proteus. We write our own versions; the waveforms are trivial.
   - Sample every control at about 5 points (Mikkonen 2024) and the input
     level at several dBu, because captures are level-dependent.
3. **Run SPICE offline** at 4–8x the target rate, band-limit, and
   decimate. This gives (input, output, control) triples.
4. **Train a NAM or RTNeural-class network** (LSTM or WaveNet, conditioned
   on the controls) with our own MIT- or BSD-based trainer.
   - Loss: ESR plus a multi-resolution STFT loss (auraloss, Apache-2.0).
   - Or skip the network and ship the WDF model directly where it runs in
     budget. That is ADR-0011 N2's own path. The network is only there to
     make a multi-stage amp cheap.
5. **Cabinet from published curves.**
   - Build a minimum-phase FIR from the maker's on-axis frequency-response
     curve.
   - Add the low-frequency alignment from T/S parameters and the cabinet
     volume: closed-box Qtc and fc; open-back as a dipole with a baffle-size
     high-pass.
   - Add **breakup** above about 2 kHz only as far as the published curve
     shows it.

**What D.2 can reproduce:**
- the circuit's static and dynamic nonlinearity, as published;
- tone-stack interaction;
- power-amp clipping into a realistic load;
- sag, to the extent the PSU is modelled;
- the on-axis magnitude response and the LF alignment of a speaker.

**What it cannot reproduce:**
- **Microphone and placement.** The off-axis colouration, cap-versus-edge
  and distance (§E). A maker's curve is usually a single on-axis
  measurement, at 1 m, in a baffle, not in the cabinet.
- **Cone breakup detail.** Fine structure and phase above about 2–3 kHz.
  The published curve is smoothed, and its phase is not published.
- **Speaker nonlinearity at volume**: Bl(x), Cms(x), voice-coil heating and
  power compression. Klippel-style large-signal parameters are not
  published for guitar drivers.
- **Room.**
- **Transformer core nonlinearity and real parasitics.** Values are not
  published.
- **The specific unit's** drift and ageing.

### D.3 Accuracy limits and how to validate later

**Sources of error, before any measurement:**
- component tolerances: carbon-comp resistors ±10–20 %, electrolytics
  −20/+80 % historically;
- tube spread: two 12AX7s from one batch differ visibly in gm;
- ageing of capacitors and tubes;
- unpublished transformer data;
- speaker unit spread (the third-party T/S disagreement above);
- and the schematic itself. Production units diverged from the drawings,
  and many archive schematics are hand-redrawn.

**Validation, in order of cost:**
1. **Sanity against the literature.**
   - Our tone-stack magnitude must match Yeh and Smith's and Kuehnel's
     published curves at the same settings.
   - The triode stage must match Koren's and Dempwolf's published curves.
   - Pure maths; it gates everything.
2. **Public CC-licensed reference recordings** of amps of the same circuit
   class, where any exist. **EGFxSet's combo-amp reverb** clips are the
   only CC BY recordings of a tube amp found, and they are reverb, not
   gain. A gap.
3. **A few real captures** (ADR-0011 N3: owned or rented):
   - reamp the same DI through the unit and through the virtual capture;
   - report ESR, an STFT distance and blind ABX;
   - fit tolerances (tube gm, transformer inductance) to the real unit if
     the error is systematic.

   **One or two real units per circuit family validate the whole virtual
   family.** That is the cost argument for D.2.
4. **Speakers.**
   - Measure one owned speaker's impedance curve (cheap: a resistor and
     the UMC1820) and compare it with the T/S-derived curve.
   - Measure its near-field response against the maker's chart.

### D.4 Legal (an engineering reading; counsel confirms)

- **Usable facts:**
  - circuit topology, component values and published parameters;
  - T/S tables, frequency-response values read off a chart;
  - cabinet dimensions;
  - patent disclosures (expired or not: reading a patent is lawful;
    *practising* a live claim is not).
  - All four patents named in this note **need review** before we rely on
    their status: US 4,211,893 and US 4,701,957 (Google lists both as
    expired) and US 8,666,090 and US 8,284,952 (Google lists both as active).
    See Open question 10.

  Functional behaviour is not protected expression (*SAS Institute v World
  Programming*, C-406/10; `native-plugin-suite.md` §5).
- **Copyrighted expression we must not redistribute:**
  - schematic **drawings**, service-manual **text and photographs**,
    datasheet **PDFs**, chart images, Koren's model **files**.
  - We may read them and transcribe the facts into our own netlists and
    tables. We keep no copies in the repository, and we cite the source in
    the capture card (ADR-0011 §4.6).
- **Trademarks.** Marshall, Fender, Bassman, Mesa/Boogie, Rectifier, Mark,
  Orange, Celestion, Vintage 30, Jensen and so on are never used in product,
  capture, preset or UI names (ADR-0011 §5).
  - Descriptions are circuit-class only, e.g.:
    - "British 1960s EL34 head, plexi-era voicing";
    - "American 1950s tweed 4×10 combo circuit";
    - "Californian 1980s cascaded-gain lead circuit";
    - "1980s British high-gain head";
    - "12-inch British ceramic speaker, 75 Hz resonance".
  - Naming the original in marketing is the owner's call with counsel
    (ADR-0011 "For the owner", item 2).
- **Trade dress.** Not affected: the editors use Sonora's visual language
  (ADR-0011 §5).

---

## E. Microphone modelling

**Spec and literature: what a mic model must capture.**
1. **On-axis magnitude response.** Published by makers. Example: the Shure
   SM57 guide gives 40 Hz–15 kHz and a typical response curve, and states
   proximity boost of roughly 6–10 dB below 100 Hz at about 6 mm
   (https://www.mcgill.ca/library/files/library/sm57-user-guide.pdf; via
   search). Sensitivity differs between sheet revisions (−56.0 vs
   −54.5 dBV/Pa).
2. **Polar pattern versus frequency.** Makers publish polar plots at a few
   frequencies. That is enough for a first-order model: pressure (omni)
   plus pressure-gradient (figure-8) mix, per band.
3. **Proximity effect.** Physics, not data. For a first-order gradient
   capsule at distance r, LF gain rises with 1/(kr), so it can be derived
   (a derivation is in T. Deppisch's Graz student project,
   https://phaidra.kug.ac.at/open/o:106765, via search).
4. **Off-axis colouration and placement on a cabinet** (cap, cap-edge,
   cone, distance) are the speaker's radiation pattern times the mic's
   polar response. The speaker's radiation at angle is **not published**.
   Measure our own, or use a piston-in-baffle approximation.
5. **Nonlinear and transient behaviour**: ribbon and transformer
   saturation, dynamic-capsule mass. Second order, rarely modelled.

**Approaches:**
- **(a) Filter matching** (`native-plugin-suite.md` §2.2). Matching mic A
  to mic B is an EQ that is the ratio of their responses at the source
  angle. Cheap, linear, ours.
- **(b) Dual-capsule beamforming.**
  - Townsend Labs, **US 8,666,090** "Microphone modeling system and method"
    (https://patents.google.com/patent/US8666090): a dual-capsule mic, with
    filters optimised so on-axis follows a target mic and off-axis follows
    an ideal pattern, and proximity compensation.
  - Current assignee per Google: Universal Audio. Status **Active,
    anticipated expiry 2033-02-26**.
  - **Do not implement the claimed method** before a patent review.
- **(c) Multi-capsule directivity weighting.**
  - **US 8,284,952** (AKG), active, expires 2029-10-13 per Google
    (https://patents.google.com/patent/US8284952).
  - Also a patent-review item.
- **(d) Neural mic or cab models.**
  - No open neural microphone-emulation paper or repo was found this
    session (searched DAFx and arXiv terms).
  - NAM captures sometimes include the mic in a cab capture; that is
    capture-specific.

**Open-licence mic response data:**
- **None found that is verified shippable.**
- Franco Hernández, Bacila, Brookes, De Sena, "A Multi-Angle, Multi-Distance
  Dataset of Microphone Impulse Responses", JAES 70(10), 2022
  (https://aes2.org/publications/elibrary-page/?id=22014): 25 mics plus a
  Class-1 reference; 0–355° in 5° steps; 0.5, 1.25 and 5 m. **Its data
  location and licence were not found.** Ask the authors (Surrey) before
  anything else; it is the right dataset if open.
- Manufacturer curves are read off images, as facts (D.4). Keep no copies.

**Our route:**
- linear models only: magnitude plus first-order polar plus derived
  proximity, from published curves;
- measurement of our own mic locker with the Farina sweep;
- the user's own reference for "sounds like mic X".

**Trademark rule:** no mic brand or model names (Shure, SM57, Neumann, U 87,
Royer, AKG and so on) in product, preset or UI. Use "large-diaphragm
condenser, cardioid", "moving-coil dynamic, presence peak", "ribbon,
figure-8".

---

## F. Open-source neural audio DSP on GitHub

Licences read from the GitHub API on 2026-10-13. RT means real-time
capability as published, not measured here.

| Repo | Licence | Implements | Maturity | RT | Weights and data | Copy? |
|---|---|---|---|---|---|---|
| **sdatkinson/neural-amp-modeler** (trainer) | MIT | WaveNet and LSTM amp training, `.nam` export | 3.1k★, active | n/a (training) | No shipped data; trains on your capture | **Copy now** |
| **sdatkinson/NeuralAmpModelerCore** | MIT | C++ inference for `.nam` (WaveNet, LSTM, ConvNet), Eigen | Active (2026-10) | Yes (published; plugin users run it live) | — | **Copy now**. N3 candidate |
| **sdatkinson/NeuralAmpModelerPlugin** | MIT | iPlug2 plugin around the core | Active | Yes | — | Learn; we have our own format layer |
| **mikeoliphant/neural-amp-modeler-lv2** | GPL-3.0 | LV2 wrapper for NAM | Active | Yes | — | After the ruling |
| **mikeoliphant/NeuralAudio** | MIT | Loader and runner for NAM and RTNeural-format models with static-size fast paths | Active (2026-10) | Yes | — | **Copy now**; worth reading for preallocated inference |
| **jatinchowdhury18/RTNeural** | BSD-3-Clause | Header-only RT inference: Dense, GRU, LSTM, Conv1D, compile-time sizes | 850★, active | **Yes**, designed for it (arXiv 2106.03037) | — | **Copy now**. N3 candidate |
| **AidaDSP/AIDA-X** | GPL-3.0-or-later (README) | LV2, VST3 and CLAP amp-model player plus cab IR, on RTNeural | Active to 2024-12 | Yes (MOD hardware) | One bundled model and IR; packs on Google Drive, no licence stated | After the ruling; **models excluded** |
| **AidaDSP/Automated-GuitarAmpModelling** | GPL-3.0 | AIDA-X trainer fork | 2025 | n/a | — | After the ruling |
| **Alec-Wright/Automated-GuitarAmpModelling** | GPL-3.0 | Reference LSTM and GRU trainer from the DAFx-19 and Appl. Sci. 2020 papers | 2023 | n/a | — | After the ruling; or reimplement from the paper |
| **Alec-Wright/CoreAudioML** | GPL-3.0 | Training library used above | 2023 | n/a | — | After the ruling |
| **Alec-Wright/NeuralTimeVaryFx** | GPL-3.0 | Neural modelling of time-varying effects (phaser, flanger) | 2021 | — | — | Learn |
| **GuitarML/Proteus** | GPL-3.0 | LSTM-40 amp and pedal player, knob conditioning | 2023 | Yes | ToneLibrary (A.1): provenance unclear | After the ruling; models excluded |
| **GuitarML/SmartGuitarAmp** | **Apache-2.0** | WaveNet amp plugin (small tube amp, clean and drive) | 2023 | Yes | Bundled model of "a small tube amp" (README); unit, provenance and model terms not documented | Code **copy now**; model excluded |
| **GuitarML/PedalNetRT** | GPL-3.0 | WaveNet pedal trainer | 2023 | n/a | — | After the ruling |
| **GuitarML/NeuralPi**, **SmartAmpPro**, **TS-M1N3**, **Chameleon** | GPL-3.0 | Pi-based player, CNN+LSTM amp, single-pedal and single-amp models | 2022–24 | Yes | Captures of named commercial units: excluded | After the ruling (code) |
| **GuitarML/GuitarLSTM** | GPL-3.0 | Keras LSTM trainer | 2022 | n/a | — | Learn |
| **jatinchowdhury18/KlonCentaur** | BSD-3 | WDF plus RNN trained on SPICE output | 2021 | Yes | **BSD-3, SPICE-derived**: clean | **Copy now**; the §D template |
| **jatinchowdhury18/AnalogTapeModel** | GPL-3.0 | Physical tape model, plus optional neural hysteresis | 2025 | Yes | — | After the ruling |
| **jatinchowdhury18/differentiable-wdfs** | BSD-3 | Differentiable WDFs, to fit circuit models to data | 2022 | n/a | — | **Copy now**; a grey-box fit of D.3 tolerances |
| **Chowdhury-DSP/chowdsp_wdf** | BSD-3 | WDF runtime | Active | Yes | — | **Copy now** |
| **magenta/ddsp** | Apache-2.0 | Differentiable oscillators, filters and reverb; timbre transfer | Active | Partly (ddsp-vst, archived, Apache-2.0) | Training data of the released checkpoints **not checked** this session | Code copy now; weights excluded until checked |
| **csteinmetz1/auraloss** | Apache-2.0 | ESR, multi-resolution STFT and other audio losses | Stable | n/a | — | **Copy now** (training) |
| **csteinmetz1/dasp-pytorch** | Apache-2.0 | Differentiable EQ, compressor, reverb and so on | 2023 | n/a | — | **Copy now** (training) |
| **csteinmetz1/micro-tcn** | Apache-2.0 | TCN compressor model (LA-2A, SignalTrain) | 2022 | Yes (TorchScript, C++) | SignalTrain CC BY; weights see A.7 | **Copy now** |
| **csteinmetz1/steerable-nafx**, **ronn**, **IIRNet** | Apache-2.0 | Random-network effects, randomised-overdrive nets, IIR design by network | 2022–24 | Mixed | — | Copy now (research quality) |
| **csteinmetz1/automix-toolkit** | Apache-2.0 | Automatic mixing models and training | 2024 | n/a | Training data of any released weights **not checked** | Code copy now; weights excluded until checked |
| **drscotthawley/signaltrain** | GPL-3.0 | SignalTrain compressor profiling code | 2023 | — | — | After the ruling |
| **mcomunita/nablafx** | MIT | Framework for differentiable black-, grey- and white-box audio effect models | 2025 | n/a | — | **Copy now** |
| **mcomunita/gcn-tfilm** | MIT | Gated conv nets with FiLM for time-varying effects | 2023 | Partly | — | Copy now |
| **sh-lee97/grafx** | Apache-2.0 | Differentiable audio processing graphs (mix-console estimation) | Active (2026) | n/a | — | **Copy now**; relevant to the mix assistant |
| **gdalsanto/flamo** | MIT | Differentiable FDN and LTI toolbox | Active (2026-09) | n/a | — | **Copy now**; fit our FDN reverb (N2) to IRs |
| **francescopapaleo/neural-audio-spring-reverb** | **AGPL-3.0** | Neural spring-reverb models | 2026 | — | — | **Learn only** |
| **Neutone/neutone_sdk** | LGPL-2.1 | Wrap PyTorch models for a real-time host plugin | 2025 | Yes (in their host) | Many community models trained on unknown data | Learn. ADR-0011 forbids a general runtime in `process` |
| **pytorch/audio** (torchaudio) | BSD-2-Clause | Audio I/O, transforms, some models | Active | n/a | Model weights per model | Copy now (training) |
| **RAVE** | CC BY-NC 4.0 (`native-plugin-suite.md` §4) | Realtime VAE | — | Yes | — | **No** |
| **DamRsn/NeuralNote** | Apache-2.0 | Audio-to-MIDI plugin around Basic Pitch | Active | Yes | Basic Pitch weights: data includes MedleyDB (A.7) | Code copy now; weights no |

**Patterns in this survey:**
- **Permissive code with clean weights is rare.** KlonCentaur (SPICE data)
  and micro-TCN (SignalTrain CC BY) are the two clean cases.
- In every other case the weights are absent, undocumented, or trained on
  non-commercial data.
- That supports ADR-0011's design: we train our own small models (amp,
  pitch) on data we can account for, and use the permissive training tools
  (NAM trainer, auraloss, dasp-pytorch, nablafx, grafx, flamo).

---

## Open questions (not determined here)

1. Whether ADR-0011 §4.6's "rendered through a commercial plugin" rule
   covers Slakh, StemGMD and E-GMD, which are MIDI rendered through
   commercial sample instruments (A.4 note 1). This is the
   architect's and owner's question.
2. The VST3 SDK's licence (ADR-0011 Unverified 1). Not re-checked here.
3. Whether any TONE3000 uploader offers a CC licence on a capture.
4. The data location and licence of the Surrey microphone IR dataset.
5. Whether Celestion publishes full T/S for guitar drivers. The official
   page was not reached.
6. The current official Fender schematic URL, and whether Marshall or
   Orange publish service data.
7. The licence headers of x42 `meters.lv2` (GPL-2.0-only would be
   incompatible).
8. The terms of the MIT IR Survey, the 800 extra Demucs songs, URMP and
   RMVPE's training data.
9. The licence of Common Voice's *audio*. Only its sentence text is
   confirmed CC0 (A.6).
10. A patent search on pitch correction (carried over from
    `native-plugin-suite.md` §5), plus a review of every patent named here:
    - US 8,666,090 and US 8,284,952, for mic modelling; Google lists both
      as active;
    - US 4,211,893 and US 4,701,957, the Mesa amplifier patents; Google lists
      both as expired (anticipated 1998-11-13 and 2006-11-26), which is
      Google's assumption, not a legal conclusion.
11. GPL-3's "preferred form of modification" for shipped weights
    (`native-plugin-suite.md` §3). This note assumes we publish the
    training code and a data recipe.
