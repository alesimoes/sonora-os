# ADR-0146 - Template categories and newer-than-build in the template list

- Status: Accepted (recommended defaults, taken by the engine agent with no
  owner available, 2026-11-09)
- Date: 2026-11-09
- Amends: ADR-0030 §4 (categories, "a follow-up") and §5 (the version read
  without a load)
- Item: `daw.model.session_mgmt` (the template browser part)
- Numbering: 146 is reserved by the coordinator for this item. No session
  format version and no protocol version is used.

## Summary

*Navigation only; the sections below are the decision.*

- A template category is **one subfolder of the templates folder**: one
  level, no nesting, its name checked as a session name is.
- `model::TemplateInfo` (`session_template.h`) is a template browser's
  entry: `name`, `file`, `category` (empty = uncategorised),
  `file_format_version` and `newer_than_build`. Every field has a default.
- The version is read with `file_format_version()` (the first key only),
  never a full load. A newer template is listed greyed, not hidden.
- No wire change, no protocol change, no session format change. Rename
  through the engine and any new `ModelAction` are deferred.

## Context

ADR-0030 kept templates in a per-user folder laid out like the sessions
folder, `<templates>/<Name>/<Name>.sonora`, listed by `list_sessions()`. It
left categories (Pro Tools' template groups) as a follow-up, and said in §5
that `file_format_version()` lets a browser grey out a template a newer
build wrote. Neither had a type to carry it: the browser held a
`SessionEntry` (name and file), so a category and the version check had no
place to live, and each caller would have invented its own.

The template browser needs both. An operator with forty templates sorts
them into groups (Tracking, Mixing, Podcast). A stick that was used by a
newer image and then booted on an older one has templates the older loader
refuses; listing them as if they would open, then refusing at New from
Template, is the worse experience.

## Decision

### 1. A category is one subfolder of the templates folder

    <templates>/<Name>/<Name>.sonora                  uncategorised
    <templates>/<Category>/<Name>/<Name>.sonora       in <Category>

- One level only. A folder under a category is a template folder or
  nothing; there is no category inside a category.
- A folder directly under `<templates>` is a template when it holds
  `<Name>.sonora`, and a category otherwise. A template folder is never a
  category, so a template called "Mixing" and a category called "Mixing"
  cannot both exist; the list shows the template.
- A category's name passes the same check as a session name
  (`session_name_problem()`, ui/qt), so it is a folder name on every
  filesystem the stick may be read on.
- An empty category is not listed; the operator makes one by saving a
  template into it.

### 2. `TemplateInfo`

In `common/model/include/sonora/model/session_template.h`:

| Field | Type | Default | Meaning |
|---|---|---|---|
| `name` | `std::string` | empty | the template folder's name |
| `file` | `std::string` | empty | its `.sonora` file |
| `category` | `std::string` | empty | the subfolder it is in; empty = uncategorised |
| `file_format_version` | `std::int64_t` | 0 | what the file declares; 0 until read |
| `newer_than_build` | `bool` | false | declared version above `kFormatVersion` |

`read_template_version(bytes, info)` fills the last two from the file's
bytes; `format_newer_than_build(version)` is the comparison. The model does
no I/O: the lister (ui/qt `session_files`) reads the folder and the bytes.

### 3. The version is read from the head, never by a load

`file_format_version()` reads the first key, `sonora_session`, and nothing
else: no migration, no invariant, no sequence file. Bytes that are not a
session file make `read_template_version()` return false and leave the
entry untouched, and the lister leaves that folder out, as
`list_sessions()` does for a folder without its file. A newer template is
listed, marked, and New from Template on it is not offered; an older one
is listed as any other and migrated in memory when used (ADR-0030 §5).

### 4. What is deferred

Renaming or moving a template between categories through the engine, and
any new `ModelAction` for templates, need a protocol number and are not
part of this decision. Save as Template into a category is the existing
Save Copy In with a path one level deeper; New from Template is the
existing prefixed `Load` (ADR-0030, amendment of 2026-11-04).

## Rationale

- A folder is what the operator sees in a file manager and what a copy to
  another stick keeps. A category key inside the file would need a format
  bump and would disagree with the folder the file sits in.
- One level matches Pro Tools' template groups and keeps the list a flat
  two-level tree the browser can show without a tree widget.
- Reading only the version keeps listing forty templates cheap and never
  runs a migration on a file the operator only looked at.
- Defaults on every field keep every existing brace-initialiser and caller
  compiling, so the contract lands ahead of its users.

## Consequences

- `common/model` gains `TemplateInfo`, `format_newer_than_build()` and
  `read_template_version()`, header-only, and
  `tests/test_session_template_info.cpp`.
- The ui/qt lister (`list_templates()` over the two levels) and the browser
  with categories and greyed entries are the ui-engineer's slice of this
  item, built on this contract.
- No session format, protocol or wire change; `contracts/api` is untouched.
- A template saved by a newer build stays on disk untouched; nothing in an
  older build writes over it unless the operator saves a template of the
  same name.

## Alternatives rejected

- **A `"category"` key in the session file.** A format bump, and the file
  and its folder could disagree.
- **Nested categories.** No DAW workflow we reproduce needs them, and the
  browser and the name rules would have to handle depth.
- **Hiding newer templates.** The operator would think them lost; greyed
  with the reason is honest and costs nothing.
- **Reading the version with `deserialize()`.** A full load per entry, a
  migration on a file only listed, and a refusal where a mark is wanted.
- **A `ListTemplates` / `RenameTemplate` engine action now.** Needs a
  protocol number and the engine has no reason to read the templates
  folder; deferred (§4).
