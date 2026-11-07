"""What the dashboard reads from GitHub through `gh`: open PRs with their
checks and today's merges. Read-only; empty on failure."""
import json
import re
import time

from dashboard_util import run

STEP_CACHE = {}


def failing_step(job):
    # A finished job never changes, so one lookup per job id is enough.
    if job not in STEP_CACHE:
        try:
            j = json.loads(run(["gh", "api", "repos/{owner}/{repo}/actions/jobs/%s" % job]) or "{}")
        except Exception:
            j = {}
        if not j:
            return None
        STEP_CACHE[job] = next((st.get("name") for st in j.get("steps", []) if st.get("conclusion") == "failure"), "") or ""
    return STEP_CACHE[job]


def read_prs():
    out = run(["gh", "pr", "list", "--state", "open", "--limit", "60", "--json",
               "number,title,headRefName,baseRefName,mergeStateStatus,mergeable,createdAt,isDraft,statusCheckRollup"])
    prs = []
    for p in json.loads(out or "[]"):
        failed, pending, passed, cancelled, details = [], [], 0, [], []
        for c in p.get("statusCheckRollup") or []:
            name = c.get("name") or c.get("context") or "?"
            if c.get("status") not in (None, "COMPLETED"):
                pending.append(name)
            elif c.get("conclusion") in ("FAILURE", "CANCELLED", "TIMED_OUT", "STARTUP_FAILURE"):
                failed.append(name)
                if c.get("conclusion") == "CANCELLED":
                    cancelled.append(name)
                m = re.search(r"/job/(\d+)", c.get("detailsUrl") or "")
                details.append({"name": name, "conclusion": c.get("conclusion"),
                                "step": failing_step(m.group(1)) if m and c.get("conclusion") != "CANCELLED" else None})
            elif c.get("conclusion") in ("SUCCESS", "SKIPPED", "NEUTRAL"):
                passed += 1
        prs.append({"number": p["number"], "title": p["title"], "branch": p["headRefName"],
                    "merge": p.get("mergeStateStatus"), "created": p["createdAt"],
                    "failed": failed, "pending": pending, "passed": passed,
                    "base": p.get("baseRefName"), "draft": bool(p.get("isDraft")), "mergeable": p.get("mergeable"),
                    "cancelled": cancelled, "failed_details": details,
                    "total": passed + len(failed) + len(pending)})
    return sorted(prs, key=lambda p: p["number"])


def read_merged():
    # "Today" is the local day: the UTC date rolls over in the evening.
    t = time.localtime()
    start = time.mktime((t.tm_year, t.tm_mon, t.tm_mday, 0, 0, 0, 0, 0, -1))
    since = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(start))
    out = run(["gh", "pr", "list", "--state", "merged", "--limit", "100", "--search", "merged:>=" + since,
               "--json", "number,title,mergedAt"])
    rows = [p for p in json.loads(out or "[]") if p["mergedAt"] >= since]
    return sorted(rows, key=lambda p: p["mergedAt"], reverse=True)
