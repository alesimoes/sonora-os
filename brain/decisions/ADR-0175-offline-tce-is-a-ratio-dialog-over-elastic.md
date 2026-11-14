# ADR-0175: Offline TCE is a ratio dialog over elastic

## Summary
Time compression/expansion adds no engine verb. The dialog (`ui/tce`) turns a
target (length, seconds, bars, tempo change, percent) into a ratio, refuses
one outside `kMinElasticRatio..kMaxElasticRatio` (0.25..4) or past
`kMaxElasticFrames` before sending, and sends `elastic ratio R ALGO` through
ui/elastic's command path. Pitch stays the separate pitch verb.

## Decision
- Pure calculator `tce_ratio`: new length / old length; a faster tempo is a
  shorter region (source_bpm / target_bpm).
- `TceDialog::apply` refuses first, then calls `elastic_command`. The send
  path carries no reply body, so the length shown is the accepted ratio's.
- Own CMake; ui/elastic is included, not edited.
