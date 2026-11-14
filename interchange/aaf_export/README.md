# AAF export (ADR-0161)

`AafExporter` maps a session to a neutral `AafDocument`, copies each external
source beside the `.aaf` as 32-bit float WAV (a missing source is an error
naming it), then hands the document to an `AafWriter`.

`make_aaf_exporter()` returns the exporter for this build. **libaaf is
read-only** (its author says it cannot create or modify AAF), so no build can
write AAF through it yet: without `SONORA_WITH_LIBAAF` the write fails with
"built without AAF support", with it ON "libaaf is read-only". Never an empty
file. A real writer implements `AafWriter` and replaces
`src/aaf_libaaf_writer.cpp`.

Not wired into `sonora-convert` yet (follow-up).
