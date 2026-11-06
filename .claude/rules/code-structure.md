# Code structure

- Files stay under about 500 lines (guideline). A change that pushes a file
  past it splits the file in the same PR or says why not. Why: 3k-line files;
  two model files hung the ASan build for 40 minutes.
- One domain per file; small functions; no god headers. Add new operations or
  types in a new header, not at the end of `model.h` / `operations.h`.
- Dependencies go in through interfaces (constructor injection), so tests use
  fakes. No new globals or singletons.
- A header that everything includes stays light: forward-declare, do not grow
  its `std::variant` list (the model's `Operation`/`Member` variants are the
  known compile-time trap, `common/model/include/sonora/model/operations.h`).
- Modules obey the dependency DAG (ADR-0007; `tools/module-deps.allow`).
- Refactor and feature never share a PR or a commit (`refactor-engineer`).
- Real-time path rules stay in `CLAUDE.md` ("Rules for the real-time path");
  every RT bug fix gets its regression test first.
- Comments say why, never what. C++17, 4-space indent, `snake_case_` members.
