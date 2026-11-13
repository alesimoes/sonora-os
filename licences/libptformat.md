# libptformat

Third-party library used to read Pro Tools `.ptx` sessions (ADR-0156).

| | |
|---|---|
| Upstream | https://github.com/zamaudio/ptformat |
| Pinned revision | `af15ee90825f59b84b9977591862cce96bb1d781` (master, 2026-06-14; upstream has no release tags past `legacy`) |
| Copyright | 2015-2019 Damien Zammit, 2015-2019 Robin Gareus |
| Licence | GNU Lesser General Public License, version 2.1 or (at your option) any later version |
| Licence text | `COPYING.LESSER` in the upstream tree; https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html |
| Built by | `tools/Dockerfile.ptformat` (from source, at the pinned revision) |

The licence is the one stated in `ptformat.cc` and `COPYING.LESSER` at the
pinned revision. It is a GPL-family licence and compatible with Sonora's
GPL-3.0-or-later; the owner accepted it on 2026-11-13 (ADR-0156).

## Where it is linked

Only into the `sonora-convert` process (ADR-0156, ADR-0007 4.5), through the
optional `sonora_ptx_libptformat` adapter built with
`-DSONORA_WITH_LIBPTFORMAT=ON`. It is never linked into the engine, the
real-time path or the user interface. It is a shared library
(`libptformat.so.0`), so a user can replace it with a modified build.

## Corresponding source

The exact source of the library Sonora ships is the upstream tree at the
pinned revision above. It is also provided:

- beside the binary: the build image keeps the tree it built from as
  `/usr/local/src/libptformat-<revision>.tar.gz`, and an image that ships
  `sonora-convert` with libptformat ships that archive, this file and the
  licence text under `/usr/share/doc/libptformat/`;
- on request: for three years from the date we distribute a binary containing
  libptformat, anyone may ask the SonoraOS maintainers, through the project's
  issue tracker, for a copy of the complete corresponding source on a medium
  customarily used for software interchange, for no more than the cost of
  physically performing the distribution.

No modification is made to the upstream source; the build compiles
`ptformat.cc` unchanged into `libptformat.so.0` and writes a `ptformat.pc`.
