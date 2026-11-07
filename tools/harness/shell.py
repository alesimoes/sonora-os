"""Run repo shell scripts through the right bash.

The daemon on Windows is started without Git's usr/bin on PATH, so a bare
"sh" is not found (2026-11-07); a bare "bash" may be WSL's, which reads none
of the C:/ paths it is given."""
import os
import shutil
import sys


def bash():
    if sys.platform != "win32":
        return "bash"
    git = shutil.which("git")
    if git:
        base = os.path.dirname(os.path.dirname(git))
        for rel in (("bin", "bash.exe"), ("usr", "bin", "bash.exe")):
            cand = os.path.join(base, *rel)
            if os.path.exists(cand):
                return cand
    return os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"),
                        "Git", "bin", "bash.exe")
