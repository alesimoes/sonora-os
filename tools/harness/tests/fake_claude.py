"""Stand-in for `claude -p` (ADR-0047 section 7). Never contacts anything.

Behaviour per launch comes from $FAKE_CLAUDE_DIR/<agent name>: one mode per
line, the n-th launch of that name reads line n (the last line repeats).
Without a file the mode is $FAKE_CLAUDE_DEFAULT or "done".

Modes: done | fail | blocked | bug | invalid | hang | die | limit |
       sleep:<seconds>   (then done)
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
        time.sleep(float(mode.split(":")[1]))
        mode = "done"
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
