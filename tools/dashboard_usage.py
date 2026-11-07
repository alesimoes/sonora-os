"""Usage snapshots written by the status line script."""
import json
import os
import time

def read_usage():
    """Snapshots written by the status line script, one file per session."""
    d = os.path.join(os.path.expanduser("~"), ".claude", "dashboard-usage")
    rows = []
    try:
        for n in os.listdir(d):
            if n.endswith(".json"):
                try:
                    rows.append(json.load(open(os.path.join(d, n), encoding="utf-8")))
                except Exception:
                    pass
    except OSError:
        return {"sessions": [], "week": None, "five_hour": None}
    now = time.time()
    rows = [r for r in rows if now - r.get("ts", 0) < 6 * 3600]
    rows.sort(key=lambda r: r.get("ts", 0), reverse=True)
    pick = lambda k: next((r[k] for r in rows if r.get(k)), None)
    return {"sessions": rows, "week": pick("seven_day"), "five_hour": pick("five_hour")}
