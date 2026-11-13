"""What the agents cost: the day's spend, by model, from the `result` line at
the end of each run's stream.log (owner 2026-11-09: "por que estamos queimando
tanto token"). A run whose log has not changed is not read again, so the
refresh loop stays cheap with a thousand runs on disk.
"""
import json
import os
import re
import time

WINDOW_S = 24 * 3600
TAIL_BYTES = 40000
_cache = {}      # run dir -> (mtime_ns, {family: usd}, total usd)


def family(model):
    m = re.sub(r"^claude-", "", str(model))
    return re.sub(r"-\d.*$", "", m) or "?"


def run_cost(path):
    """({family: usd}, total usd) from the result line in the log's tail."""
    try:
        with open(path, "rb") as f:
            f.seek(0, 2)
            f.seek(max(0, f.tell() - TAIL_BYTES))
            tail = f.read().decode("utf-8", "replace")
    except OSError:
        return {}, 0.0
    for line in reversed(tail.splitlines()):
        if '"type":"result"' not in line:
            continue
        try:
            r = json.loads(line)
        except ValueError:
            return {}, 0.0
        by = {}
        for model, u in (r.get("modelUsage") or {}).items():
            by[family(model)] = by.get(family(model), 0.0) + float(u.get("costUSD") or 0)
        return by, float(r.get("total_cost_usd") or sum(by.values()))
    return {}, 0.0


def read_cost(runs_dir, now=None):
    """{"usd", "by_model": {family: usd}, "top": {"run", "usd"}} for the last day."""
    now = now or time.time()
    total, by_model, top = 0.0, {}, {"run": "", "usd": 0.0}
    try:
        names = os.listdir(runs_dir)
    except OSError:
        return None
    for name in names:
        log = os.path.join(runs_dir, name, "stream.log")
        try:
            st = os.stat(log)
        except OSError:
            continue
        if now - st.st_mtime > WINDOW_S:
            _cache.pop(name, None)
            continue
        hit = _cache.get(name)
        if hit is None or hit[0] != st.st_mtime_ns:
            by, usd = run_cost(log)
            hit = _cache[name] = (st.st_mtime_ns, by, usd)
        total += hit[2]
        for fam, usd in hit[1].items():
            by_model[fam] = by_model.get(fam, 0.0) + usd
        if hit[2] > top["usd"]:
            top = {"run": re.sub(r"-r\d+$", "", name), "usd": hit[2]}
    return {"usd": round(total, 2), "top": dict(top, usd=round(top["usd"], 2)),
            "by_model": {k: round(v, 2) for k, v in sorted(by_model.items(), key=lambda kv: -kv[1])}}
