"""Shared by the dashboard readers: the repo root, running a command that may
fail, and where the orchestrator's files are."""
import os
import subprocess

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def run(cmd, timeout=40):
    try:
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=timeout)
        return (r.stdout or "") if r.returncode == 0 else ""
    except Exception:
        return ""


def norm(path):
    return path.replace("\\", "/").rstrip("/").lower()


def main_root(root=ROOT):
    """The main checkout: when the dashboard runs from an agent worktree
    (<main>/.claude/worktrees/<id>) the repo's real root is above it."""
    parts = os.path.abspath(root).replace("\\", "/").split("/.claude/worktrees/")
    return parts[0] if len(parts) > 1 else os.path.abspath(root)


def orch_status_path():
    return os.environ.get("SONORA_ORCH_STATUS") or os.path.join(ROOT, "planning", "orchestrator", "status.json")


def orch_root():
    """The orchestrator's checkout: three levels above its status.json."""
    return os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(orch_status_path()))))
