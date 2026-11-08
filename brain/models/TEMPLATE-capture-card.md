# Capture card: TEMPLATE (copy to `brain/models/<id>.md`)

Copy this file to `brain/models/<id>.md`, where `<id>` matches `^[a-z0-9-]+$`
and is the card's file stem, then replace every placeholder. The card exists
before the capture or IR it describes does (ADR-0011 §4.6, ADR-0069 §1).

Everything outside the ```card-json block is free prose: rig notes, listening
notes. Keep the prose free of brands, artist names and model numbers
(ADR-0011 §4.6, §5). The shipped name is the `name` field; the deny check
reads `id`, `name` and `file.name`. The lessor's name stays with the rental
papers, not on the card.

The block below is the machine part. It must be exactly one block, opened by
a line that reads ` ```card-json ` and closed by a line of three backticks.
Unknown keys are refused, so do not add fields.

```card-json
{
  "id": "placeholder-id",
  "name": "placeholder name, named by its sound and circuit class",
  "kind": "capture",
  "source": "hardware",
  "unit": {
    "type": "placeholder: the real unit, for provenance (not shipped)",
    "serial": "placeholder-serial",
    "ownership": "owned",
    "rental_terms_allow_recording": false
  },
  "settings": {
    "gain": "5",
    "bass": "5",
    "middle": "5",
    "treble": "5",
    "channel": "placeholder-channel"
  },
  "rig": "placeholder: reamp box, load or attenuator, interface and sample rate",
  "date": "2000-01-01",
  "third_party": null,
  "capture": {
    "architecture": "placeholder-architecture",
    "sample_rate": 48000,
    "esr_held_out": 0.0,
    "train_commit": "0000000000000000000000000000000000000000"
  },
  "file": {
    "name": "placeholder-id.nam",
    "sha256": "0000000000000000000000000000000000000000000000000000000000000000",
    "size": 0
  }
}
```

## Notes for the author

- `kind` is `capture` for an amp, or `ir` for a cabinet. An `ir` card takes
  `"ir": {"cabinet", "speaker", "microphone", "position", "room"}` in place of
  `capture` (ADR-0069 §2). The example above is a capture card.
- `ownership` is `owned` or `rented`. A rented unit needs
  `rental_terms_allow_recording: true`, and the checklist in
  `brain/research/ampcab-capture-rig.md` §8 done first.
- `third_party` is `null` for our own work. Anything else needs a
  `licence_path` to a written licence that allows redistribution.
- `esr_held_out` is the error-to-signal ratio on held-out audio, defined in
  the research note §6. A capture without it is refused by policy.
- `sha256` and `size` are the file's, computed after the file is final. The
  placeholder values here are refused by the build check.
- `date` is `YYYY-MM-DD`, the day of the recording session.
- The placeholder id, name, date, serial and file hash must all be replaced.
  `parse_card` accepts the template as it stands; `validate_card` refuses it
  until they are.
