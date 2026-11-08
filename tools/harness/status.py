"""status.json for the dashboard (ADR-0047 section 6): written atomically,
read-only for everyone else."""
import json
import os
import time

from . import blockinfo, leadwake
from .run_view import run_view
from .scheduler import POOL_OF_KIND, ready_feature_ids

DONE_STATES = ("integrating", "pr_open", "ci_fix", "merged")
# The dashboard colours a task by matching its `state` text against
# /done|ok|merged|x|feito/ (green), /block|wait|fail|red/ (red) and
# /doing|work|run|progress|building/ (blue). Raw states such as ci_fix or
# bugfix contain an `x` and would read as done, so tasks carry one of these
# words in `state` and the real state in `stage`.
DASH_STATE = {"queued": "queued", "building": "building",
              "bugfix": "working", "qa": "working", "integrating": "done",
              "pr_open": "done", "merged": "done", "ci_fix": "red",
              "blocked": "blocked", "closed": "closed"}


def replace_retry(src, dst, tries=5, delay=0.2):
    """os.replace, retried on Windows when the dashboard has the target
    open (PermissionError)."""
    for n in range(tries):
        try:
            os.replace(src, dst)
            return
        except PermissionError:
            if n == tries - 1:
                raise
            time.sleep(delay)


def write_atomic(path, data):
    tmp = f"{path}.tmp.{os.getpid()}"
    try:
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=1, sort_keys=True)
            f.flush()
            os.fsync(f.fileno())
        replace_retry(tmp, path)
    except BaseException:
        try:
            os.remove(tmp)
        except OSError:
            pass
        raise


def build(store, cfg, now, runs, gaps, mode, stop, stop_now, usage_pause_until,
          paused, hour_runs, planner_runs=0, holds=None):
    """runs: active-run dicts already enriched with working/building."""
    tasks = store.tasks()
    by_id = {t["id"]: t for t in tasks}
    pools = {}
    for kind, pool in POOL_OF_KIND.items():
        mine = [r for r in runs if r["role"] == kind]
        pools[pool] = {"floor": 0, "live": len(mine),
                       "working": sum(1 for r in mine if r["working"]),
                       "paused": pool in paused}
    for track, floor in sorted(cfg.floors.items()):
        mine = [r for r in runs if r["role"] == "engineer"
                and by_id.get(r["task_id"], {}).get("track") == track]
        pools[f"build:{track}"] = {
            "floor": floor, "live": len(mine),
            "working": sum(1 for r in mine if r["working"]),
            "reason": gaps.get(track), "paused": track in paused}
    by_feat = {f["id"]: f for f in store.features()}
    blocked = blockinfo.blocked_groups(tasks, by_feat)
    feats = []
    for f in store.features():
        mine = [t for t in tasks if t["feature_id"] == f["id"]
                and t["kind"] == "work"]
        why = next((t["note"] for t in mine
                    if t["state"] == "blocked" and t["note"]), None)
        feats.append({
            # dashboard fields: name, state, pr, tasks[].{name,state}
            "name": f["id"], "id": f["id"], "track": f["track"],
            "state": f["state"], "pr": f["pr"],
            "tasks": [{"name": t["id"], "state": DASH_STATE[t["state"]],
                       "stage": t["state"], "agent": t["agent"],
                       "engineer": t["eng_name"], "attempts": t["attempts"],
                       "bounces": t["bounces"]} for t in mine],
            "fixes": f["fixes"], "tasks_done": sum(
                1 for t in mine if t["state"] in DONE_STATES),
            "tasks_total": len(mine),
            "adr": f["adr_no"], "protocol": f["protocol_no"],
            "partial": bool(f["partial"]), "age": now - f["created"],
            "blocked_reason": why if f["state"] == "blocked" else None})
    owner = blockinfo.owner_items(tasks, by_feat)
    return {
        "updated": time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(now)),
        "updated_ts": now, "mode": mode,
        "kill_switch": "STOP.NOW" if stop_now else "STOP" if stop else None,
        "usage_pause_until": usage_pause_until or None,
        "cap": cfg.cap, "live": len(runs), "paused": sorted(paused),
        "runs_last_hour": hour_runs,
        "max_runs_per_hour": getattr(cfg, "max_runs_per_hour", None),
        "planner_runs_last_hour": planner_runs,
        "max_planner_runs_per_hour": getattr(
            cfg, "max_planner_runs_per_hour", None),
        "budget_holds": dict(holds or {}),
        "max_principals": getattr(cfg, "max_principals", None),
        "lead_wakes_last_hour": leadwake.count_last_hour(
            store.tasks(kind="wake"), now),
        "open_prs": len({t["feature_id"] for t in tasks
                         if t["state"] in ("pr_open", "ci_fix")}),
        "max_open_prs": cfg.max_open_prs,
        "ready_unpr": len(ready_feature_ids(tasks)),
        "max_ready_unpr": cfg.max_ready_unpr,
        "pools": pools,
        "runs": [{"id": r["id"], "task": r["task_id"], "role": r["role"],
                  "name": r["name"], "pid": r["pid"], "started": r["started"],
                  "last_beat": r["last_beat"], "working": r["working"],
                  "building": r["building"], "suspect": bool(r["suspect"]),
                  **run_view(r, by_id.get(r["task_id"]))}
                 for r in runs],
        "tasks": [{"id": t["id"], "state": t["state"], "track": t["track"],
                   "kind": t["kind"], "attempts": t["attempts"],
                   "pr": t["pr"], "branch": t["branch"], "age": now
                   - t["created"]} for t in tasks],
        "features": feats,
        "blocked": blocked,
        "waits_on_owner": owner,
        "privileged_pending": [t["id"] for t in tasks
                               if "needs_privileged" in (t["brief"] or "")
                               and t["state"] == "qa"],
        "reservations": store.reservations(),
    }
