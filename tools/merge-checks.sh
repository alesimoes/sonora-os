#!/bin/sh
# Decision helpers for tools/merge-when-green.sh, sourced and tested apart
# (tools/test-merge-checks.sh).
#
# The heavy CI jobs have `needs: changes`, so GitHub creates their check runs
# only after "Changed paths" finishes. A poll in that window sees only the
# cheap jobs, all green. A merge needs every heavy check present, passed or
# skipped (a job skipped by `if:` still reports one, bucket "skipping").

HEAVY_CHECKS="Engine build and tests
Portable modules under MSVC
Qt interface build and headless tests
Thread and address sanitizers
RealtimeSanitizer over the SONORA_RT call graph
Records audio against PipeWire
BWF files read back by third-party tools
Quantum and rate stay pinned
Plugin host against real LV2 and VST3 plugins"

# stdin: "name<TAB>bucket" lines. Prints each heavy check that is absent or
# not pass/skipping; prints nothing when all are present and fine.
missing_heavy_checks() {
  checks=$(cat)
  printf '%s\n' "$HEAVY_CHECKS" | while IFS= read -r want; do
    printf '%s\n' "$checks" | awk -F'\t' -v w="$want" \
      '$1 == w && ($2 == "pass" || $2 == "skipping") { found = 1 } END { exit !found }' ||
      printf '%s\n' "$want"
  done
}
