# Shipped captures and impulse responses

Every amp capture (`.nam`) and cabinet IR (`.wav`) we ship needs a capture
card and an entry in `manifest.json` (ADR-0069, ADR-0011 s4.6). The capture
files themselves are not in git; the manifest pins them by SHA-256 and size.
Only recordings of real hardware we own or may record, or third-party files
with a written licence on the card, are accepted. Never a plugin's output.

## Adding one

1. Record it following the capture rig procedure in `brain/specs/`.
2. Pick an id (`^[a-z0-9-]+$`) and a name that describes the sound. No brand,
   model number or artist (`tools/ampcab_name_deny.txt`).
3. Write `brain/models/<id>.md` with one fenced `card-json` block
   (`format_card` in `capture_card.h` prints the shape). Fill `file.sha256`
   and `file.size` from the file.
4. Add `{ "id", "version", "sha256", "size", "card" }` to `entries`, with the
   same hash and size as the card; `card` is `brain/models/<id>.md`.
5. Put the file in this directory, then run `ampcab-card-check`. It fails on
   a bad card, a hash or size mismatch, a file above 25 MB, a total above
   100 MB, or a capture or IR file here that no card names.
