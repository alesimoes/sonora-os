"""The daemon's runtime files across a move of its checkout (2026-11-14).

A plan commit in the daemon checkout tracked state.json and events.jsonl;
the forced self-update left that commit for origin/main and git deleted
both files, so the daemon restarted on an empty store (68 features and
228 tasks became 15 and 30; owner waits, ADR reservations and history
lost). git deletes a file when it leaves a commit that tracks it, and a
cherry-pick or checkout overwrites an ignored file a commit carries.

So before any reset, checkout, rebase or replay the runtime files are read
into memory and copied aside on disk (.harness/selfupdate-aside/runtime-*,
the last KEEP_ASIDE kept); afterwards every one git removed or changed is
written back. An events file that only grew (the live daemon appended
meanwhile) is left as it is.
"""
import os
import shutil
import tempfile
import time

from . import runtime_files

ASIDE = os.path.join(".harness", "selfupdate-aside")
KEEP_ASIDE = 3


def _read(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except OSError:
        return None


def _write_atomic(path, data):
    d = os.path.dirname(path)
    os.makedirs(d, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=d, prefix=".restore-", suffix=".tmp")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def _grew_only(rel, before, now):
    return rel.endswith(".jsonl") and now is not None and \
        len(now) > len(before) and now.startswith(before)


def at_risk(root, git):
    """The core runtime files plus every runtime file HEAD tracks."""
    rels = set(runtime_files.CORE)
    if git is not None:
        r = git(["ls-tree", "-r", "-z", "--name-only", "HEAD"], root)
        if not r.returncode:
            rels.update(runtime_files.runtime_only(
                [f for f in r.stdout.split("\0") if f]))
    return sorted(rels)


class Runtime:
    """Snapshot taken at construction; restore() puts back what git took."""

    def __init__(self, root, git=None, now=None):
        self.root, self.saved, self.aside = root, {}, None
        for rel in at_risk(root, git):
            data = _read(os.path.join(root, rel))
            if data is not None:
                self.saved[rel] = data
        if git is not None:   # a real move: keep a copy on disk as well
            self.aside = self._copy_aside(now if now is not None
                                          else time.time())

    def _copy_aside(self, now):
        base = os.path.join(self.root, ASIDE)
        stamp = time.strftime("%Y%m%d-%H%M%S", time.localtime(now))
        dest = os.path.join(base, f"runtime-{stamp}")
        for rel, data in self.saved.items():
            _write_atomic(os.path.join(dest, rel), data)
        self._prune(base)
        return dest

    @staticmethod
    def _prune(base):
        try:
            old = sorted(n for n in os.listdir(base)
                         if n.startswith("runtime-"))
        except OSError:
            return
        for n in old[:-KEEP_ASIDE]:
            shutil.rmtree(os.path.join(base, n), ignore_errors=True)

    def restore(self):
        """Writes back each saved file git removed or changed; returns
        their paths (relative to the checkout)."""
        put_back = []
        for rel, data in sorted(self.saved.items()):
            path = os.path.join(self.root, rel)
            now = _read(path)
            if now == data or _grew_only(rel, data, now):
                continue
            _write_atomic(path, data)
            put_back.append(rel)
        return put_back
