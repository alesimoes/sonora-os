"""Shared test fixtures: path setup, a stub broker, fake worktrees, clock."""
import os
import shutil
import sys
import tempfile
import time
import unittest

TOOLS = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
if TOOLS not in sys.path:
    sys.path.insert(0, TOOLS)
FAKE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                    "fake_claude.py")

from harness import scheduler  # noqa: E402
from harness.forge import Forge, ForgeError  # noqa: E402
import orchestrator  # noqa: E402


class StubBroker:
    """The BrokerHost surface the daemon uses, recording calls (the real
    broker is exercised in test_integration)."""
    port = 4242
    token = "stub-token"

    def __init__(self):
        self.started = self.stopped = False
        self.sent = []
        self.expected = []
        self.channels = {}
        self.claims = {}
        self.events = []

    def start(self):
        self.started = True

    def stop(self):
        self.stopped = True

    def drain_events(self):
        out, self.events = self.events, []
        return out

    def expect(self, *names):
        self.expected.extend(names)

    def open_channel(self, feature, members):
        self.channels[feature] = set(members)

    def claim_for(self, owner, paths):
        self.claims[owner] = list(paths)
        return []

    def release_for(self, owner, paths=()):
        return int(self.claims.pop(owner, None) is not None)

    def tell(self, to, type_, body=None, feature=None, task=None):
        self.sent.append((to, type_, body))

    def emit(self, kind, **data):
        self.events.append((kind, data))


class FakeForge(Forge):
    """git/gh in memory. `fail` names methods that raise ForgeError."""

    def __init__(self):
        self.prs = {}          # number -> {branch, files, state, mergeable, checks}
        self.next_pr = 100
        self.main_paths = []
        self.main = {}         # path -> text
        self.diffs = {}        # pr -> diff text
        self.pushed, self.enqueued, self.created, self.pre_pr_calls = \
            [], [], [], []
        self.pre_pr_result = (True, "ok")
        self.fail = set()

    def _chk(self, what):
        if what in self.fail:
            raise ForgeError(f"{what} failed")

    def open_prs(self):
        self._chk("open_prs")
        return [{"number": n, "branch": p["branch"], "files": p["files"]}
                for n, p in self.prs.items() if p["state"] == "open"]

    def pr_diff(self, pr):
        self._chk("pr_diff")
        return self.diffs.get(pr, "")

    def main_files(self, prefix):
        self._chk("main_files")
        return [p for p in self.main_paths if p.startswith(prefix)]

    def main_text(self, path):
        self._chk("main_text")
        return self.main.get(path, "")

    def pr_status(self, pr):
        self._chk("pr_status")
        p = self.prs[pr]
        return {"state": p["state"], "mergeable": p["mergeable"],
                "checks": [{"name": k, "state": v}
                           for k, v in sorted(p["checks"].items())]}

    def push(self, worktree, branch):
        self._chk("push")
        self.pushed.append(branch)

    def create_pr(self, branch, base, title, body):
        self._chk("create_pr")
        n = self.next_pr
        self.next_pr += 1
        self.prs[n] = {"branch": branch, "files": [], "state": "open",
                       "mergeable": "yes", "checks": {"build": "pending"}}
        self.created.append({"number": n, "branch": branch, "base": base,
                             "title": title, "body": body})
        return n

    def enqueue(self, pr):
        self._chk("enqueue")
        self.enqueued.append(pr)

    def pre_pr(self, worktree):
        self._chk("pre_pr")
        self.pre_pr_calls.append(worktree)
        return self.pre_pr_result

    def checks(self, pr, **states):
        self.prs[pr]["checks"] = dict(states)

    def merge(self, pr, head_sha=None):
        self._chk("merge")
        p = self.prs[pr]
        if head_sha is not None and head_sha != p.get("head", "h0"):
            raise ForgeError("head moved")
        p["state"] = "merged"
        self.merged = getattr(self, "merged", []) + [(pr, head_sha)]

    # -- merge train --------------------------------------------------
    def add_pr(self, n, branch, files=(), base="main", checks=None,
               mergeable="yes", title="", draft=False):
        self.prs[n] = {"branch": branch, "files": list(files), "base": base,
                       "state": "open", "mergeable": mergeable,
                       "checks": dict(checks or {}), "head": "h0", "draft": draft,
                       "title": title or branch}

    def list_prs(self):
        self._chk("list_prs")
        return [{"number": n, "title": p.get("title", ""),
                 "branch": p["branch"], "base": p.get("base", "main"),
                 "draft": p.get("draft", False),
                 "files": p["files"]}
                for n, p in sorted(self.prs.items()) if p["state"] == "open"]

    def pr_detail(self, pr):
        self._chk("pr_detail")
        p = self.prs[pr]
        return {"state": p["state"], "mergeable": p["mergeable"],
                "base": p.get("base", "main"), "branch": p["branch"],
                "head_sha": p.get("head", "h0"),
                "checks": [{"name": k, "state": v}
                           for k, v in sorted(p["checks"].items())]}

    def _new_head(self, pr):
        p = self.prs[pr]
        n = int(p.get("head", "h0")[1:]) + 1
        p["head"] = f"h{n}"
        p["checks"] = {k: "pending" for k in p["checks"]}

    def update_branch(self, pr):
        self._chk("update_branch")
        r = self.prs[pr].get("update", "ok")
        self.updated = getattr(self, "updated", []) + [pr]
        if r == "ok":
            self._new_head(pr)
        return r

    def rerun_failed(self, pr, head_sha):
        self._chk("rerun_failed")
        self.reruns = getattr(self, "reruns", []) + [(pr, head_sha)]
        p = self.prs[pr]
        p["checks"] = {k: ("pending" if v in ("fail", "cancel") else v)
                       for k, v in p["checks"].items()}
        return True

    def retarget(self, pr, base):
        self._chk("retarget")
        self.prs[pr]["base"] = base

    def push_head(self, worktree, branch):
        self._chk("push")
        self.pushed.append(branch)
        n = next((k for k, p in self.prs.items() if p["branch"] == branch
                  and p["state"] == "open"), None)
        if n is not None and getattr(self, "fix_commits", True):
            self._new_head(n)

    def close_pr(self, pr, comment):
        self._chk("close_pr")
        self.prs[pr]["state"] = "closed"
        self.closed = getattr(self, "closed", []) + [(pr, comment)]

    def contains(self, worktree, branch):
        return branch not in getattr(self, "left_out", ())


class DirWorktrees:
    """Plain directories; the git paths have their own test."""

    def __init__(self, root):
        self.root = root

    def ensure(self, task_id, branch, base=None):
        self.bases = getattr(self, "bases", {})
        self.bases[task_id] = base
        p = os.path.join(self.root, "wt", task_id)
        os.makedirs(p, exist_ok=True)
        return p

    def path(self, task_id):
        return os.path.join(self.root, "wt", task_id)

    def ensure_pr(self, key, branch, start=None):
        self.pr_checkouts = getattr(self, "pr_checkouts", []) + [branch]
        self.starts = getattr(self, "starts", []) + [start]
        return self.ensure(key, branch)

    def remove(self, task_id):
        pass


class Clock:
    def __init__(self):
        self.offset = 0.0

    def __call__(self):
        return time.time() + self.offset


class DaemonCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="orch-")
        self.fake_dir = os.path.join(self.tmp, "fake")
        os.makedirs(self.fake_dir)
        os.environ["FAKE_CLAUDE_DIR"] = self.fake_dir
        os.environ.pop("FAKE_CLAUDE_DEFAULT", None)
        self.clock = Clock()
        self.orchs = []

    def tearDown(self):
        for o in self.orchs:
            o._kill_all("teardown")
            o.close()
        os.environ.pop("FAKE_CLAUDE_DIR", None)
        shutil.rmtree(self.tmp, ignore_errors=True)

    def script(self, name, *modes):
        with open(os.path.join(self.fake_dir, name), "w") as f:
            f.write("\n".join(modes))

    def make(self, dry_run=False, floors=None, broker=None, seed_fn="fixed",
             **cfg):
        c = scheduler.Config(cap=cfg.pop("cap", 20),
                             floors=floors or {"daw": 2, "os": 1},
                             backoff_s=(0, 0, 0), local_builds=3, **cfg)
        self.forge = FakeForge()
        o = orchestrator.Orchestrator(
            self.tmp, cfg=c, broker=broker or StubBroker(), dry_run=dry_run,
            fake_claude=FAKE, clock=self.clock, forge=self.forge,
            worktrees=DirWorktrees(self.tmp),
            seed_fn=(lambda k: 10) if seed_fn == "fixed" else seed_fn)
        self.orchs.append(o)
        return o

    def pump(self, o, until, timeout=15):
        end = time.time() + timeout
        while time.time() < end:
            o.tick()
            if until():
                return True
            time.sleep(0.02)
        o.tick()
        return until()
