"""Version and ADR numbers the daemon reserves (ADR-0047 4a step 1).

The seed of each kind is the highest value on origin/main or in any open
PR's diff (and the configured floor), so a reservation never collides with
work that is already in flight. Agents never choose a number.
"""
import re

# kind -> (file that defines it, pattern with the number in group 1)
SOURCES = {
    "protocol": ("contracts/api/include/sonora/api/messages.h",
                 r"kProtocolVersion\s*=\s*(\d+)"),
    "format": ("common/model/include/sonora/model/session_file.h",
               r"kFormatVersion\s*=\s*(\d+)"),
    "window": ("common/model/src/op_codec.cpp",
               r"kWindowFormat\s*=\s*(\d+)"),
}
ADR_DIR = "brain/decisions"
_ADR = re.compile(r"ADR-(\d+)")


def seed(kind, forge, configured=0):
    """Highest number in use for `kind`. Raises ForgeError if a read fails:
    guessing would risk a duplicate."""
    best = int(configured)
    prs = forge.open_prs()
    if kind == "adr":
        paths = list(forge.main_files(ADR_DIR))
        for pr in prs:
            paths += [p for p in pr["files"] if p.startswith(ADR_DIR + "/")]
        for p in paths:
            best = max([best] + [int(n) for n in _ADR.findall(p)])
        return best
    path, pat = SOURCES[kind]
    m = re.search(pat, forge.main_text(path))
    if m:
        best = max(best, int(m.group(1)))
    added = re.compile(r"^\+.*?" + pat, re.M)
    for pr in prs:
        if path in pr["files"]:
            for n in added.findall(forge.pr_diff(pr["number"])):
                best = max(best, int(n))
    return best
