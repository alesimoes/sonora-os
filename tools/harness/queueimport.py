"""Reads planning/AGENT-QUEUE.md (the V1 seed) into feature requests.

The queue is a markdown table: Order | Item | Agent | Waits on. One row is one
feature; an Item may name several BACKLOG ids. MIDI, video, immersive and
virtual instruments are out of v1 and skipped (OUT_OF_V1).
"""
import re

# Owner 2026-11-13: AAF export is not built (libaaf only reads), so the
# export and the AAF round trip are out too; AAF import stays.
OUT_OF_V1 = re.compile(r"midi|video|immersive|surround|instrument"
                       r"|interchange\.sessions\.(?:aaf_export|roundtrip)\b",
                       re.I)
OUT_OF_V1_TEXT = ("MIDI, virtual instruments, video, immersive/surround, "
                  "AAF export and the AAF round trip "
                  "(interchange.sessions.aaf_export, "
                  "interchange.sessions.roundtrip)")
BACKLOG_ID = re.compile(r"[a-z][a-z0-9_]*(?:\.[a-z0-9_]+)+")
TRACK_OF_AGENT = (("dsp-engineer", "plugin"), ("plugin-engineer", "plugin"),
                  ("distro-engineer", "os"), ("driver-engineer", "os"))


def out_of_scope(text):
    return bool(OUT_OF_V1.search(text))


def track_of(agent):
    for key, track in TRACK_OF_AGENT:
        if key in agent:
            return track
    return "daw"


def slug(text):
    return re.sub(r"[^a-z0-9]+", "-", text.lower()).strip("-")[:48] or "item"


def parse(text):
    """[{id, track, backlog_ids, brief, waits_on, order}] in queue order."""
    rows = []
    for line in text.splitlines():
        cells = [c.strip() for c in line.strip().strip("|").split("|")]
        if len(cells) < 3 or not cells[0].isdigit():
            continue
        item, agent = cells[1], cells[2]
        waits = cells[3] if len(cells) > 3 else ""
        if out_of_scope(item) or out_of_scope(agent):
            continue
        bare = re.sub(r"\([^)]*\)", " ", item)   # drop "(EQ, dynamics...)"
        ids = list(dict.fromkeys(BACKLOG_ID.findall(bare)))
        fid = ids[0] if ids else slug(bare)
        rows.append({"id": fid, "track": track_of(agent),
                     "backlog_ids": ids or [fid], "brief": item,
                     "waits_on": waits, "order": int(cells[0])})
    return rows
