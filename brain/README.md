# Brain

Durable knowledge. Anything an agent would otherwise have to rediscover by
reading the whole tree belongs here. Conversation context dies at the end of a
session; this does not.

| Folder | Holds | Written by |
|---|---|---|
| `decisions/` | ADRs. One architectural decision each, immutable once accepted. | architect |
| `research/` | Findings about formats, protocols, kernels, hardware. Cites sources. | researcher |
| `specs/` | Condensed working notes on a file format or protocol we implement. | researcher |
| `postmortems/` | What broke, the root cause, the regression test that now guards it. | whoever fixed it |

## Rules

1. **One subject per file.** If a file needs "and" in its title, split it.
2. **Cite the source.** A research note without a URL or a spec section number
   is a rumour. Mark anything measured locally as measured, with the hardware.
3. **State what is unverified.** "ALSA reports 32 channels" and "we tested 32
   channels at 64 frames" are different claims. Never promote one to the other.
4. **ADRs are append-only.** To reverse a decision, write a new ADR that
   supersedes the old one and set `Superseded by:` in the original header.
5. **No secrets, no scratch work.** This is a public repository.

## Index

Keep `brain/INDEX.md` current — it is what agents read first to decide which
files to open.
