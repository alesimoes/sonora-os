---
name: split-big-file
description: Split a file over about 500 lines by domain without changing behaviour. Use before adding to a big file, or when asked to refactor one.
---

# Split a big file

1. Measure: `wc -l FILE`; list its domains (group declarations by what
   changes together). Name the new files after the domain.
2. Run the module's tests first; note the result (the baseline).
3. Commit 1 - move only: cut each group into its new file, byte for byte. The
   old header keeps `#include`s of the new ones, so no includer changes.
4. Commit 2 - fix includes: forward declarations over includes; add includes
   where a file needs them. Keep `static_assert`s next to what they check.
5. Commit 3 (optional) - interfaces: extract an abstract class for the
   collaborator, inject it through the constructor, keep the concrete one
   as default in the factory.
6. Run the same tests; they must pass unchanged. Public symbols and include
   paths are identical.
7. Report line counts before/after and any file still over 500 lines.
8. Do not mix a bug fix or feature in. Do not append to a big variant list
   in a header (`.claude/context/model-contracts.md`).
