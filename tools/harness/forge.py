"""Everything the daemon asks of git, gh and the repo scripts (ADR-0047 4a, 5).

One interface, so the orchestrator never shells out for these itself and the
tests swap in a fake. GhForge is the real thing; it raises ForgeError when a
read fails, because a number seeded from a half-read repo could collide.
"""
import hashlib
import json
import os
import re
import subprocess
import tempfile

from harness import branch_tip, forge_push, prepr, shell


class ForgeError(Exception):
    pass


class ForgeConflict(ForgeError):
    """The branch conflicts with its remote copy: for the integrator, not a
    retry."""

    def __init__(self, msg, paths):
        super().__init__(msg)
        self.paths = list(paths)


class RateLimited(ForgeError):
    """GitHub refused for rate (primary, secondary or abuse limit): every gh
    call waits, not only the one that failed."""


RATE_PATTERNS = ("rate limit", "secondary rate", "abuse detection",
                 "HTTP 429", "was submitted too quickly")


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

    def pre_pr(self, worktree, env=None):
        """(ok, output). A repo without the script passes with a note. `env`:
        extra variables for the script (harness/prepr.allow_env)."""
        raise NotImplementedError

    # -- merge train (adopted PRs) ------------------------------------
    def list_prs(self):
        """[{number, title, branch, base, files}] for every open PR."""
        raise NotImplementedError

    def pr_detail(self, pr):
        """pr_status plus base, head_sha, and check states pass|fail|cancel|
        pending (cancel kept apart: a cancelled run is rerun, not fixed)."""
        raise NotImplementedError

    def update_branch(self, pr):
        """Merge main into the PR branch on GitHub: ok|uptodate|conflict."""
        raise NotImplementedError

    def rerun_failed(self, pr, head_sha):
        """Rerun the failed and cancelled jobs of the head's latest CI run.
        False when there is no run to rerun."""
        raise NotImplementedError

    def rerun_job(self, pr, head_sha, job_name):
        """Rerun one failed job (by its check name) of the head's latest
        finished CI run. False when there is no such failed job."""
        raise NotImplementedError

    def merge(self, pr, head_sha):
        """Squash-merge, refused by GitHub if the head moved."""
        raise NotImplementedError

    def retarget(self, pr, base):
        raise NotImplementedError

    def push_head(self, worktree, branch):
        """Push the worktree's HEAD to `branch` (adopted PRs are checked out
        on a local train/ branch, so another worktree may hold the name)."""
        raise NotImplementedError

    def commit_all(self, worktree, message):
        """Stage and commit every change in `worktree`."""
        raise NotImplementedError

    def close_pr(self, pr, comment):
        raise NotImplementedError

    def failure_excerpt(self, head_sha, per_job=8):
        return ""

    def set_status(self, sha, state, context, description):
        """A commit status on GitHub."""

    def workflow_runs(self, workflow, branch="main", limit=3):
        """[{id, head_sha, status, conclusion, url}] newest first, for the
        workflow file on the branch (the nightly full build)."""
        return []

    def dispatch_workflow(self, workflow, ref="main"):
        """Start a workflow_dispatch run of the workflow on the ref."""
        raise NotImplementedError

    def contains(self, worktree, branch):
        """True when origin/<branch>'s head is in the worktree's HEAD (a
        bundle took that PR)."""
        raise NotImplementedError

    def is_ancestor(self, sha, head):
        """True when commit `sha` is an ancestor of (or equal to) `head`."""
        raise NotImplementedError

    def branch_tip(self, branch):
        """{sha, ts (committer epoch seconds), subject, ref} of the branch
        head that counts: the local one, or origin's when it is strictly
        ahead (a hand fix pushed from another worktree, harness/branch_tip.py).
        None when no such branch exists."""
        raise NotImplementedError

    def sync_local(self, branch):
        """Fast-forward the local branch to origin's head. (ok, why)."""
        raise NotImplementedError

    def changed_files(self, base, branch):
        """Paths `branch` changed since `base` (harness/branch_tip.py), or
        None when that cannot be told."""
        raise NotImplementedError


def job_signatures(excerpt):
    """{job name: cause} from failure_excerpt's text. The cause is the job's
    error lines with numbers, hashes and paths of temp dirs blurred, so the
    same failure on a new head or at another line still compares equal."""
    out, job, lines = {}, None, []

    def close():
        if job is not None:
            norm = sorted({re.sub(r"[0-9a-f]{7,}|\d+", "#", ln)
                           for ln in lines})
            out[job] = hashlib.sha1(
                "\n".join(norm).encode()).hexdigest()[:12]
    for ln in (excerpt or "").splitlines():
        m = re.fullmatch(r"\[(.+)\]", ln.strip())
        if m:
            close()
            job, lines = m.group(1), []
        elif job is not None and ln.strip():
            lines.append(ln.strip())
    close()
    return out


def run(argv, cwd=None, timeout=120, env=None):
    """`env` adds to the daemon's environment, never replaces it."""
    try:
        return subprocess.run(argv, cwd=cwd, capture_output=True, text=True,
                              timeout=timeout, encoding="utf-8",
                              errors="replace",
                              env=dict(os.environ, **env) if env else None)
    except (OSError, subprocess.SubprocessError) as e:
        raise ForgeError(f"{argv[0]}: {e}")


class GhForge(Forge):
    def __init__(self, root, runner=run, base_ref="origin/main"):
        self.root, self.run, self.base_ref = root, runner, base_ref

    def _ok(self, argv, cwd=None, timeout=120):
        r = self.run(argv, cwd or self.root, timeout)
        if r.returncode:
            # git prints CONFLICT lines on stdout: keep both streams.
            self._raise(argv, r.stderr, r.stdout)
        return r.stdout

    @staticmethod
    def _raise(argv, stderr, stdout=""):
        text = (stderr or "").strip()
        if (stdout or "").strip():
            text = (text + " " + stdout.strip()).strip()
        msg = f"{' '.join(argv[:3])}: {text[:600]}"
        if any(p.lower() in stderr.lower() for p in RATE_PATTERNS):
            raise RateLimited(msg)
        raise ForgeError(msg)

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
        def ok(argv, cwd, timeout):
            return self._ok(argv, cwd, timeout)
        try:
            return forge_push.push_feature_branch(ok, worktree, branch,
                                                  self.base_ref)
        except forge_push.MergeConflict as e:
            raise ForgeConflict(str(e), e.paths)

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
        self._ok([shell.bash(), "tools/enqueue-pr.sh", str(pr)], timeout=60)

    def list_prs(self):
        out = self._ok(["gh", "pr", "list", "--state", "open", "--limit",
                        "100", "--json",
                        "number,title,headRefName,baseRefName,files,isDraft"],
                       timeout=60)
        try:
            rows = json.loads(out or "[]")
        except ValueError as e:
            raise ForgeError(f"gh pr list: {e}")
        return [{"number": r["number"], "title": r.get("title", ""),
                 "branch": r.get("headRefName"), "base": r.get("baseRefName"),
                 "draft": bool(r.get("isDraft")),
                 "files": [f["path"] for f in r.get("files", [])]}
                for r in rows]

    def pr_detail(self, pr):
        try:
            v = json.loads(self._ok(
                ["gh", "pr", "view", str(pr), "--json",
                 "state,mergeable,baseRefName,headRefName,headRefOid,"
                 "statusCheckRollup"], timeout=60) or "{}")
        except ValueError as e:
            raise ForgeError(f"gh pr view: {e}")
        checks = []
        for c in v.get("statusCheckRollup") or []:
            name = c.get("name") or c.get("context") or "?"
            if c.get("status") not in (None, "COMPLETED"):
                st = "pending"
            else:
                st = {"SUCCESS": "pass", "SKIPPED": "pass", "NEUTRAL": "pass",
                      "CANCELLED": "cancel", "STALE": "cancel"}.get(
                          c.get("conclusion") or c.get("state"), "fail")
                if c.get("state") == "PENDING":
                    st = "pending"
            checks.append({"name": name, "state": st})
        return {"state": v.get("state", "OPEN").lower(),
                "mergeable": {"MERGEABLE": "yes", "CONFLICTING": "no"}.get(
                    v.get("mergeable"), "unknown"),
                "base": v.get("baseRefName"), "branch": v.get("headRefName"),
                "head_sha": v.get("headRefOid"), "checks": checks}

    def update_branch(self, pr):
        r = self.run(["gh", "pr", "update-branch", str(pr)], self.root, 120)
        text = r.stdout + r.stderr
        if r.returncode == 0:
            return "uptodate" if "up-to-date" in text or "up to date" in text \
                else "ok"
        if "conflict" in text.lower():
            return "conflict"
        self._raise(["gh", "pr", "update-branch"], r.stderr)

    def _finished_runs(self, head_sha):
        out = self._ok(["gh", "run", "list", "--commit", head_sha, "--limit",
                        "5", "--json", "databaseId,status,workflowName"],
                       timeout=60)
        try:
            runs = json.loads(out or "[]")
        except ValueError as e:
            raise ForgeError(f"gh run list: {e}")
        return [x for x in runs if x.get("status") == "completed"]

    def rerun_failed(self, pr, head_sha):
        done = self._finished_runs(head_sha)
        if not done:
            return False
        self._ok(["gh", "run", "rerun", str(done[0]["databaseId"]),
                  "--failed"], timeout=60)
        return True

    def rerun_job(self, pr, head_sha, job_name):
        done = self._finished_runs(head_sha)
        if not done:
            return False
        run_id = str(done[0]["databaseId"])
        try:
            jobs = json.loads(self._ok(
                ["gh", "run", "view", run_id, "--json", "jobs"],
                timeout=60) or "{}").get("jobs") or []
        except ValueError as e:
            raise ForgeError(f"gh run view: {e}")
        job = next((j for j in jobs if j.get("name") == job_name and
                    j.get("conclusion") in ("failure", "cancelled",
                                            "timed_out")), None)
        if job is None:
            return False
        self._ok(["gh", "run", "rerun", run_id, "--job",
                  str(job["databaseId"])], timeout=60)
        return True

    def merge(self, pr, head_sha):
        self._ok(["gh", "pr", "merge", str(pr), "--squash",
                  "--match-head-commit", head_sha], timeout=120)

    def retarget(self, pr, base):
        self._ok(["gh", "pr", "edit", str(pr), "--base", base], timeout=60)

    def push_head(self, worktree, branch):
        self._ok(["git", "push", "origin", f"HEAD:refs/heads/{branch}"],
                 worktree, timeout=300)

    def commit_all(self, worktree, message):
        self._ok(["git", "add", "-A"], worktree, timeout=60)
        trailer = "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
        self._ok(["git", "commit", "-q", "-m", message, "-m", trailer],
                 worktree, timeout=60)

    def close_pr(self, pr, comment):
        self._ok(["gh", "pr", "close", str(pr), "--comment", comment],
                 timeout=60)

    FAIL_LINE = re.compile(
        r"\(Failed\)|\(Subprocess aborted\)|\(Timeout\)|^(FAIL|ERROR): |"
        r"AssertionError|error: |forbidden edge|::error|##\[error\]|"
        r"\bSC\d{4}\b|No space left|\bE: ")

    def set_status(self, sha, state, context, description):
        self._ok(["gh", "api", "-X", "POST",
                  f"repos/{{owner}}/{{repo}}/statuses/{sha}",
                  "-f", f"state={state}", "-f", f"context={context}",
                  "-f", f"description={description}"], timeout=60)

    def workflow_runs(self, workflow, branch="main", limit=20):
        """Runs on `branch` started by the schedule or a dispatch only; a
        PR or another branch's run never speaks for main."""
        try:
            runs = json.loads(self._ok(
                ["gh", "run", "list", "--workflow", workflow, "--branch",
                 branch, "--limit", str(limit), "--json",
                 "databaseId,headSha,status,conclusion,url,event,headBranch,"
                 "updatedAt"], timeout=60) or "[]")
        except ValueError as e:
            raise ForgeError(f"gh run list: {e}")
        return [{"id": r.get("databaseId"), "head_sha": r.get("headSha"),
                 "status": r.get("status"), "conclusion": r.get("conclusion"),
                 "url": r.get("url"), "event": r.get("event"),
                 "branch": r.get("headBranch"),
                 "updated_at": r.get("updatedAt")}
                for r in runs
                if r.get("headBranch") == branch
                and r.get("event") in ("schedule", "workflow_dispatch")]

    def dispatch_workflow(self, workflow, ref="main"):
        self._ok(["gh", "workflow", "run", workflow, "--ref", ref],
                 timeout=60)

    def failure_excerpt(self, head_sha, per_job=8):
        """The lines that say why each failed job of the head's CI failed,
        so a fixer starts from the cause instead of skimming a log head."""
        try:
            runs = json.loads(self._ok(
                ["gh", "run", "list", "--commit", head_sha, "--json",
                 "databaseId,conclusion"], timeout=60) or "[]")
        except (ForgeError, ValueError):
            return ""
        out = []
        for run_ in runs:
            # A run cancelled by a newer push still has its failed jobs.
            if run_.get("conclusion") == "success":
                continue
            try:
                jobs = json.loads(self._ok(
                    ["gh", "api", f"repos/{{owner}}/{{repo}}/actions/runs/"
                     f"{run_['databaseId']}/jobs", "--jq",
                     "[.jobs[]|select(.conclusion==\"failure\")|"
                     "{id,name}]"], timeout=60) or "[]")
            except (ForgeError, ValueError):
                continue
            for j in jobs:
                r = self.run(["gh", "api", "--allow-escape-sequences",
                              f"repos/{{owner}}/{{repo}}/actions/jobs/"
                              f"{j['id']}/logs"], self.root, 120)
                lines, recent, tail = [], [], []
                for ln in (r.stdout or "").splitlines():
                    ln = re.sub(r"\x1b\[[0-9;]*m", "", ln)
                    ln = re.sub(r"^\S+Z ", "", ln).strip()
                    if ln.startswith("##[error]"):
                        tail = recent[-10:]   # what the failing step printed
                    if ln and not ln.startswith("##["):
                        recent.append(ln[:240])
                    if self.FAIL_LINE.search(ln) and \
                            not ln.startswith(("echo ", "+ echo")) and \
                            "Process completed with exit code" not in ln:
                        lines.append(ln[:240])
                out.append(f"[{j['name']}]")
                out += lines[-per_job:] or tail or [
                    "(no error line found; read the log)"]
        return "\n".join(out[:60])

    def contains(self, worktree, branch):
        self._ok(["git", "fetch", "-q", "origin",
                  f"+refs/heads/{branch}:refs/remotes/origin/{branch}"],
                 worktree, timeout=300)
        r = self.run(["git", "merge-base", "--is-ancestor",
                      f"origin/{branch}", "HEAD"], worktree, 60)
        return r.returncode == 0

    def is_ancestor(self, sha, head):
        r = self.run(["git", "merge-base", "--is-ancestor", sha, head],
                     self.root, 60)
        if r.returncode not in (0, 1):   # unknown commit: not an answer
            self._raise(["git", "merge-base"], r.stderr)
        return r.returncode == 0

    def branch_tip(self, branch):
        return branch_tip.branch_tip(self.run, self.root, branch)

    def sync_local(self, branch):
        return branch_tip.sync_local(self.run, self.root, branch)

    def changed_files(self, base, branch):
        return branch_tip.changed_files(self.run, self.root, base, branch)

    def pre_pr(self, worktree, env=None):
        script = os.path.join(worktree, "tools", "pre-pr.sh")
        if not os.path.exists(script):
            return True, "tools/pre-pr.sh not present: not run"
        argv = [shell.bash(), "tools/pre-pr.sh"]
        r = (self.run(argv, worktree, 1800, env=env) if env
             else self.run(argv, worktree, 1800))
        return r.returncode == 0, prepr.summary(r.stdout + r.stderr, 2000)
