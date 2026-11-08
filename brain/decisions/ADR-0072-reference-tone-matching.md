# ADR-0072: Reference-tone matching by spectral fingerprint

Status: Accepted (dsp-engineer, 2026-11-08; recommended defaults, nothing here needs the owner).

Builds on: ADR-0004 (what we refuse), ADR-0011 §3.3 (neural captures for amp and cabinet only), §3.4 (general analysis only in `sonora-analyze`), §4.1/§4.4 (AI produces parameters the operator sees and undoes), §4.6 (captures and cards), §5 (names); ADR-0066 §4–§6 (the timbre pass, the subcommand slots, and the "Artist-tone matching" deferral this ADR resolves); ADR-0069 (capture cards and the capture manifest).

## Summary

`sonora-analyze tone <reference.wav>` suggests the installed amp captures whose sound comes closest to a reference audio file **the user supplies**, plus the EQ that closes the rest of the gap. Matching is **by spectrum, never by name**: we never identify an artist, and never output or store an artist, band or product name. A suggestion carries only a capture id, a cabinet IR id, the card's amp settings and an EQ curve. Each capture has a **precomputed profile** (31 third-octave band levels, crest factor, high-band flatness) rendered from a fixed synthetic stimulus and stamped with the SHA-256 of its card; a stale profile fails the pass. The distance is a weighted L2 over bands; up to three candidates under a stated threshold are returned, otherwise "no close capture". With no capture cards installed, the shipped state until the owner names hardware, the pass exits 0 with "no captures installed". The contract is three headers in `plugins/analyze/include/sonora/analyze/` and the `tone` subcommand slot.

## Context

ADR-0066 §6 deferred "Artist-tone matching" (N6) because it needs amp captures recorded on hardware the owner provides and because no product, capture or preset name may carry a guitarist's or band's name (ADR-0004, ADR-0011 §5). ADR-0069 has since fixed what a capture card is and how captures are pinned. What remains open is how a match is made without ever naming anyone, what data it compares, where that data lives, and what the pass does today, when zero captures exist (ADR-0011 item 6: the owner has not named the hardware). The timbre pass (ADR-0066 §4) already measures a long-term third-octave curve and fits stock-EQ bands to a difference curve; this feature should reuse both rather than grow a second spectral front end.

## Decision

1. **The user supplies the reference; we never identify.** Input is an audio file the operator chooses. The report labels it with the file's name, echoed verbatim from the command line (the base name, as given) and never parsed, looked up or matched against any list. Nothing in the pass, its data or its report holds an artist, band, song or product name; there is no such list to match against. A suggestion carries exactly: `capture_id`, `ir_id` (empty when the capture already includes its cabinet), `amp_settings` (copied from the capture's card), `distance_db`, the crest and flatness deltas, the residual `eq_curve_db[31]` and the `eq_bands` fitted from it. As ADR-0066 §4, the reference is read, measured and closed; only its derived profile exists after the pass, and only in the report.

2. **The fingerprint.** `ToneProfile` (`tone_profile.h`) is: 31 band levels in dB on the ISO 266 third-octave centres, 20 Hz to 20 kHz (the timbre pass's `kThirdOctaveCentreHz`), relative to the mean of the valid bands, with a validity flag per band; `crest_db`, the peak-to-RMS ratio of the whole signal; and `flatness`, the spectral flatness (geometric over arithmetic mean of band power) of the bands at and above 2 kHz, 0 to 1. The engine slice computes it with the timbre pass's long-term-envelope code (`TimbreMeasurer`), on the reference and on the render of each capture. A reference band can be invalid (beyond Nyquist, or silent); a capture profile must be `complete` (every band valid and finite, crest >= 0, flatness in [0, 1]).

3. **Capture profiles are precomputed derived artefacts.** One JSON per capture at `plugins/native/ampcab/captures/profiles/<id>.json`:
   `{"id", "card_sha256", "ir_id", "ir_card_sha256", "stimulus", "bands": [31], "crest", "flatness"}`.
   `card_sha256` is the SHA-256 of the bytes of `brain/models/<id>.md`; `ir_id`/`ir_card_sha256` name the cabinet IR the render went through and its card's digest, both empty for a capture taken at the microphone (this pair extends the brief's field list: a capture rendered through a cabinet is not the same sound as the capture alone, so the IR is part of what the profile stands for). `stimulus` is `kToneStimulusId` (`"sonora-tone-stimulus-v1"`), a fixed synthetic signal generated in code (no third-party audio); changing it bumps the id and regenerates every profile. The parser is strict (unknown keys, short stamps, 30 bands, a null band, flatness outside [0, 1] are refused) and the writer is canonical. At load, `check_fresh` compares the stamps with the cards as they are now; **a changed card or IR card, or another stimulus, makes the profile stale and the pass fails with exit 1 naming the capture**: no skipping, no recomputing at match time, no fallback to another profile. Profiles are not computed at match time because a capture render is seconds of neural inference per capture and must not run on the operator's request path.

4. **Distance and candidates.** For the bands valid in both profiles, d_b = reference_b − capture_b; the weighted mean of d is removed (overall level is not tone), and the distance is sqrt(Σ w_b (d_b − mean)² / Σ w_b), an RMS in dB (`tone_distance_db`). Weights are 1.0 for the bands centred 80 Hz to 6.3 kHz (20 bands; the 6.3 kHz band's lower edge is 5.6 kHz, so the full-weight span covers "80 Hz to 6 kHz") and 0.25 outside. A reference with fewer than 16 of those 20 bands valid fails ("too short, quiet or narrow"). The matcher returns at most **three** suggestions with distance <= **6.0 dB** (`ToneMatchParams::max_distance_db`), nearest first, ties by id. A capture beyond the threshold is never suggested; if none is within it, the answer is empty and the report says **"no close capture"** (exit 0). The residual EQ curve is reference minus capture, smoothed to 1/3 octave and clamped to ±12 dB: exactly `match_timbre(to_timbre_curve(reference), to_timbre_curve(capture), {max_gain_db 12, smoothing_octaves 1/3, amount 1})`, whose `eq_bands` are the stock-EQ settings. Crest and flatness are reported as deltas, not in the distance: they describe drive and fizz, which the operator adjusts on the capture's own controls.

5. **No captures is the shipped behaviour.** With zero capture cards in the capture manifest, `sonora-analyze tone` exits 0 with an empty candidate list and the message **"no captures installed"**. That is how the feature ships until the owner names hardware and the first cards land.

6. **Offline, non-real-time.** Everything runs in `sonora-analyze` (ADR-0011 §3.4); it allocates and reads files, and nothing here is reachable from `Session::process()` or a plugin's `process`. Applying a suggestion is ordinary session operations the operator previews and can undo (ADR-0011 §4.1, §4.4): load the capture and IR in the amp-and-cab processor, set its controls, set the stock EQ.

7. **Cards are read, never written.** Capture cards and the capture manifest belong to ADR-0069. This feature reads a card through `sonora::ampcab::parse_card` (for the amp settings and to hash it) and edits neither cards nor the manifest. The profiles directory is a derived artefact produced by the amp-and-cab track's tooling from the cards.

8. **Report** (one JSON object, stdout or `--out`, the shape of ADR-0066 §4): `{"pass": "tone", "method": "third_octave_l2", "reference": "<file name verbatim>", "bands_hz": [31], "reference_db": [31, null where invalid], "reference_crest_db", "reference_flatness", "max_distance_db", "candidates": [{"capture_id", "ir_id", "amp_settings": {...}, "distance_db", "crest_delta_db", "flatness_delta", "eq_curve_db": [31], "eq_bands": [{"freq_hz", "gain_db", "q"}]}], "message": "no captures installed" | "no close capture" (only when candidates is empty)}`. Exit codes as ADR-0046: 0 report written (including the two empty answers), 1 the pass failed (unreadable reference, a stale or malformed profile, too few valid bands), 2 usage, 130 cancelled. Command line: `sonora-analyze tone <reference.wav> [--max-distance DB] [--out report.json]`; the profile and card directories default to the installed locations.

9. **Interfaces and file ownership** (the pattern of ADR-0066 §5). `tone_profile.h`: `ToneProfile`, `CaptureProfile`, `complete`, `parse_profile`, `profile_to_json`, `check_fresh`, `to_timbre_curve`, inline (the schema is the contract). `tone_match.h`: `ToneMatchParams`, `ToneSuggestion`, `tone_band_weight`, `tone_distance_db`, and the `ToneMatcher` interface, `match(const ToneProfile& reference, const std::vector<CaptureProfile>& library, std::vector<ToneSuggestion>& out, std::string& error) -> bool`. `tone_fakes.h`: `FakeToneMatcher`, `fake_tone_profile`, `fake_capture_profile`, `fake_stale_capture_profile`. `plugins/analyze/CMakeLists.txt` includes `cmake/tone.cmake` when it exists; `app/main.cpp` has the `tone` row calling `run_tone_command` (`app/commands.h`); the owner compiles `app/cmd_tone.cpp` when it exists, else `app/cmd_tone_stub.cpp` (exit 1, "tone: not implemented"). The contract test is `tests/test_tone_contract.cpp` (`analyze_tone_contract`). Later slices add files; none edits these.

## Rationale

- Matching by spectrum answers "which of our captures sounds like this file?" without any of the naming the ADR-0004/§5 rules forbid; there is no artist list to leak, so there is nothing to police after the fact.
- Third-octave long-term spectrum is what the timbre pass already measures and what the stock EQ can correct; reusing it means one front end, one EQ fit, and a residual the operator can apply directly.
- Removing the mean difference makes the distance about balance, not loudness, so a quiet reference and a hot capture still compare.
- Precomputed, stamped profiles make the match instant and deterministic, and the stamp turns "someone edited a card but not its profile" into a loud failure instead of a silently wrong suggestion.
- A threshold with an honest "no close capture" is better than always returning the nearest: with few captures the nearest can be far, and a forced match would misrepresent it.

## Consequences

- The engine slice adds `cmake/tone.cmake` (the classical `ToneMatcher`, the profile loader, `app/cmd_tone.cpp`, the stimulus generator) without touching this contract. Reading cards through `parse_card` and hashing them needs the edges `plugins/analyze -> plugins/native` (present) and `-> common/base` (present) in `tools/module-deps.allow`, and a link to the ampcab card library.
- The amp-and-cab track gains a tool that renders each capture with the stimulus and writes its profile; its build check should refuse a stale profile the same way the matcher does, so the image never ships one.
- Until cards exist the feature is visible but empty ("no captures installed"); that is intended, not a gap to paper over with synthetic captures.
- A profile schema change amends this ADR and `tone_profile.h` together; a new stimulus is a new id and a full regeneration.
- `plugins/analyze` tests carry no module label from the root `CMakeLists.txt`; `analyze_tone_contract` sets the label `analyze` itself until the root labels the module.

## Alternatives rejected

- **Identify the artist or song and look up a preset.** Requires a name list, outputs names, and is the naming ADR-0004 and ADR-0011 §5 forbid. Refused.
- **Ship presets "in the style of" named players.** Same rule; also no source of truth for what a player used. Refused.
- **Fit to, null-test against or train on a commercial amp plugin's output.** Black-box cloning (ADR-0004, ADR-0011 §5). Refused; our captures come from hardware under a card.
- **Compute capture profiles at match time.** Seconds of inference per capture on every request, and results that drift with the renderer. Precomputed and stamped instead.
- **Recompute or skip a stale profile silently.** Hides a provenance change; ADR-0011 §4.6 forbids fallbacks for captures. Fail loud instead.
- **A learned embedding (e.g. a pretrained audio model) as the fingerprint.** Opaque distance, a model and weights licence to clear, and no residual EQ the operator can apply. The third-octave profile is explainable and feeds the existing EQ fit; an embedding can come later as an opt-in method under ADR-0066 §1.
- **Always return the three nearest.** Misleading when none is close; replaced by the threshold and "no close capture".
- **Include crest and flatness in the distance.** Their scales differ from band dB and their weighting would be arbitrary before real captures exist; reported as deltas for now.

## Unverified

1. The 6.0 dB threshold, the 0.25 out-of-range weight and the 16-band minimum are starting values; they are tuned when the first real captures exist and their profiles can be compared with known references.
2. That the timbre pass's long-term envelope on a capture render with the synthetic stimulus is representative of the capture under a real guitar; the stimulus design (its spectrum and level, relative to a played DI) is the engine slice's to choose and measure.
3. That third-octave balance alone discriminates captures well enough: two captures with similar balance but different dynamics (crest) may tie; whether crest needs to join the distance is open until there are captures to measure.
4. The render's CPU cost per capture (it bounds how long the profile tool runs at build time); not measured.
5. Nothing here has run against a real capture: there are none (ADR-0011 item 6).
