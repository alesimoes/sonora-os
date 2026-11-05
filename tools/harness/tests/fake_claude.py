"""Stand-in for `claude -p` (ADR-0047 section 7). Never contacts anything.

Behaviour per launch comes from $FAKE_CLAUDE_DIR/<agent name>: one mode per
line, the n-th launch of that name reads line n (the last line repeats).
Without a file the mode is $FAKE_CLAUDE_DEFAULT or "done".

Modes: done | fail | blocked | bug | invalid | hang | die | limit |
       sleep:<seconds>[:<mode>]   (then done, or that mode)
       client            (register with the real broker, send
                          `done` to the daemon, then done)
       architect | architect-bad | lead | lead-bad
                         (event-aware: on plan_request write the canned plan
                          $FAKE_CLAUDE_DIR/plan-<feature>.json, on plan_ready
                          derive the assignment table from it; the -bad
                          variants write a plan with overlapping files or a
                          table with a missing row. Other events: done.)
"""
import json
import os
import sys
import time


def mode_for(name):
    d = os.environ.get("FAKE_CLAUDE_DIR")
    default = os.environ.get("FAKE_CLAUDE_DEFAULT", "done")
    if not d:
        return default
    path = os.path.join(d, name)
    count_path = path + ".count"
    n = 0
    if os.path.exists(count_path):
        with open(count_path) as f:
            n = int(f.read() or 0)
    with open(count_path, "w") as f:
        f.write(str(n + 1))
    if not os.path.exists(path):
        return default
    with open(path) as f:
        lines = [x.strip() for x in f if x.strip()]
    return lines[min(n, len(lines) - 1)] if lines else default


def _write(path, obj):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(obj, f)


def planner(mode, prompt):
    """The fake architect and lead: a file written where the prompt says."""
    import re
    feature = os.environ.get("SONORA_FEATURE")
    hdir = os.environ["SONORA_HARNESS"]
    plan_path = os.path.join(hdir, "plan", f"{feature}.plan.json")
    ev = re.search(r"^event: (\w+)", prompt, re.M)
    ev = ev.group(1) if ev else ""
    if mode.startswith("architect") and ev == "plan_request":
        with open(os.path.join(os.environ["FAKE_CLAUDE_DIR"],
                               f"plan-{feature}.json")) as f:
            plan = json.load(f)
        m = re.search(r"adr=(\d+)", prompt)
        adr = int(m.group(1)) if m else None
        if plan["reserved"].get("adr") == "auto":
            plan["reserved"]["adr"] = adr
            plan["adr"] = (f"brain/decisions/ADR-{adr:04d}-x.md"
                           if adr else None)
        if mode.endswith("-bad"):
            plan["tasks"][1]["files"] = list(plan["tasks"][0]["files"])
        _write(plan_path, plan)
    elif mode.startswith("lead") and ev == "plan_ready":
        with open(plan_path) as f:
            plan = json.load(f)
        rows = []
        for i, t in enumerate(plan["tasks"]):
            sub = lambda x: f"sub/{feature}/{x}"
            rows.append({"task": t["id"], "engineer": t["agent"],
                         "tester": "tester", "sub_branch": sub(t["id"]),
                         "order": i + 1,
                         "start_from": sub(t["depends_on"][0])
                         if t["depends_on"] else plan["feature_branch"]})
        if mode.endswith("-bad"):
            rows.pop()
        _write(os.path.join(hdir, "plan", f"{feature}.assignments.json"),
               {"version": 1, "feature": feature, "rows": rows})
    return "done"


def main(argv):
    name = os.environ["SONORA_AGENT_NAME"]
    result = os.environ["SONORA_RESULT_PATH"]
    agent = argv[argv.index("--agent") + 1] if "--agent" in argv else "?"
    d = os.environ.get("FAKE_CLAUDE_DIR")
    if d:
        with open(os.path.join(d, "argv.log"), "a") as f:
            f.write(json.dumps(argv) + "\n")
    mode = mode_for(name)
    print(json.dumps({"type": "system", "agent": agent, "name": name,
                      "mode": mode}), flush=True)
    if mode.startswith("sleep:"):
        parts = mode.split(":")      # sleep:<seconds>[:<mode afterwards>]
        time.sleep(float(parts[1]))
        mode = parts[2] if len(parts) > 2 else "done"
    if mode == "client":
        sys.path.insert(0, os.path.dirname(os.path.dirname(
            os.path.dirname(os.path.abspath(__file__)))))
        from harness.client import Client
        c = Client.from_env()
        c.connect()
        c.send("daemon", "done", {"who": c.whoami()["name"]})
        c.close()
        mode = "done"
    if mode in ("architect", "architect-bad", "lead", "lead-bad"):
        mode = planner(mode, argv[-1])
    if mode == "hang":
        while True:
            time.sleep(1)
    if mode == "die":
        return 1
    if mode == "limit":
        print("Claude AI usage limit reached, resets in 10 minutes",
              flush=True)
        return 1
    if mode == "invalid":
        with open(result, "w") as f:
            f.write("{not json")
        return 0
    body = {"role": agent, "task": os.environ.get("SONORA_TASK"),
            "status": "done", "summary": f"{name} {mode}", "commit": "0" * 7,
            "branch": "x", "findings": [], "followups": ["a follow-up"],
            "unverified": []}
    if mode in ("fail", "blocked"):
        body["status"] = mode
    if mode == "bug":
        body["findings"] = [{"severity": "blocking", "where": "a.cpp:1",
                             "text": "broken"}]
    os.makedirs(os.path.dirname(result), exist_ok=True)
    tmp = result + ".tmp"
    with open(tmp, "w") as f:
        json.dump(body, f)
    os.replace(tmp, result)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
