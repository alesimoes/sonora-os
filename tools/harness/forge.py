"""Everything the daemon asks of git, gh and the repo scripts (ADR-0047 4a, 5).

One interface, so the orchestrator never shells out for these itself and the
tests swap in a fake. GhForge is the real thing; it raises ForgeError when a
read fails, because a number seeded from a half-read repo could collide.
"""
import json
import os
import re
import subprocess
import tempfile


class ForgeError(Exception):
    pass


class Forge:
    """Interface. Reads first, then the writes the daemon performs."""

    def open_prs(self):
        """[{number, branch, files: [path]}] for every open PR."""
        raise NotImplementedError

    def pr_diff(self, pr):
        raise NotImplementedError

    def main_files(self, prefix):
        """Paths under `prefix` on origin/main."""
        raise NotImplementedError

    def main_text(self, path):
        """Contents of `path` on origin/main."""
        raise NotImplementedError

    def pr_status(self, pr):
        """{state: open|merged|closed, mergeable: yes|no|unknown,
        checks: [{name, state: pass|fail|pending}]}"""
        raise NotImplementedError

    def push(self, worktree, branch):
        raise NotImplementedError

    def create_pr(self, branch, base, title, body):
        """Open the PR and return its number."""
        raise NotImplementedError

    def enqueue(self, pr):
        raise NotImplementedError

    def pre_pr(self, worktree):
        """(ok, output). A repo without the script passes with a note."""
        raise NotImplementedError


def run(argv, cwd=None, timeout=120):
    try:
        return subprocess.run(argv, cwd=cwd, capture_output=True, text=True,
                              timeout=timeout, encoding="utf-8",
                              errors="replace")
    except (OSError, subprocess.SubprocessError) as e:
        raise ForgeError(f"{argv[0]}: {e}")


class GhForge(Forge):
    def __init__(self, root, runner=run, base_ref="origin/main"):
        self.root, self.run, self.base_ref = root, runner, base_ref

    def _ok(self, argv, cwd=None, timeout=120):
        r = self.run(argv, cwd or self.root, timeout)
        if r.returncode:
            raise ForgeError(f"{' '.join(argv[:3])}: {r.stderr.strip()[:300]}")
        return r.stdout

    def open_prs(self):
        out = self._ok(["gh", "pr", "list", "--state", "open", "--limit",
                        "100", "--json", "number,headRefName,files"],
                       timeout=60)
        try:
            rows = json.loads(out or "[]")
        except ValueError as e:
            raise ForgeError(f"gh pr list: {e}")
        return [{"number": r["number"], "branch": r.get("headRefName"),
                 "files": [f["path"] for f in r.get("files", [])]}
                for r in rows]

    def pr_diff(self, pr):
        return self._ok(["gh", "pr", "diff", str(pr)], timeout=60)

    def main_files(self, prefix):
        out = self._ok(["git", "ls-tree", "-r", "--name-only", self.base_ref,
                        prefix])
        return [x for x in out.splitlines() if x]

    def main_text(self, path):
        return self._ok(["git", "show", f"{self.base_ref}:{path}"])

    def pr_status(self, pr):
        view = json.loads(self._ok(
            ["gh", "pr", "view", str(pr), "--json", "state,mergeable"],
            timeout=60) or "{}")
        # `gh pr checks` exits 8 while checks are pending and 1 when one
        # failed; the JSON is on stdout either way.
        r = self.run(["gh", "pr", "checks", str(pr), "--json", "name,bucket"],
                     self.root, 60)
        checks = []
        if r.stdout.strip():
            try:
                for c in json.loads(r.stdout):
                    checks.append({"name": c["name"], "state": {
                        "pass": "pass", "skipping": "pass", "fail": "fail",
                        "cancel": "fail"}.get(c.get("bucket"), "pending")})
            except ValueError:
                pass
        elif r.returncode not in (0, 8) and "no checks" not in r.stderr:
            raise ForgeError("gh pr checks: " + r.stderr.strip()[:300])
        return {"state": view.get("state", "OPEN").lower(),
                "mergeable": {"MERGEABLE": "yes", "CONFLICTING": "no"}.get(
                    view.get("mergeable"), "unknown"),
                "checks": checks}

    def push(self, worktree, branch):
        self._ok(["git", "push", "origin", f"{branch}:{branch}"], worktree,
                 timeout=300)

    def create_pr(self, branch, base, title, body):
        fd, path = tempfile.mkstemp(prefix="orch-prbody-", suffix=".md")
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                f.write(body)
            out = self._ok(["gh", "pr", "create", "--base", base, "--head",
                            branch, "--title", title, "--body-file", path],
                           timeout=120)
        finally:
            os.remove(path)
        m = re.search(r"/pull/(\d+)", out)
        if not m:
            raise ForgeError("gh pr create printed no PR url: " + out[:200])
        return int(m.group(1))

    def enqueue(self, pr):
        self._ok(["sh", "tools/enqueue-pr.sh", str(pr)], timeout=60)

    def pre_pr(self, worktree):
        script = os.path.join(worktree, "tools", "pre-pr.sh")
        if not os.path.exists(script):
            return True, "tools/pre-pr.sh not present: not run"
        r = self.run(["sh", "tools/pre-pr.sh"], worktree, 1800)
        return r.returncode == 0, (r.stdout + r.stderr)[-2000:]
