"""status.json for the dashboard (ADR-0047 section 6): written atomically,
read-only for everyone else."""
import json
import os

from .scheduler import POOL_OF_KIND

DONE_STATES = ("integrating", "pr_open", "ci_fix", "merged")


def write_atomic(path, data):
    tmp = f"{path}.tmp.{os.getpid()}"
    try:
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump(data, f, indent=1, sort_keys=True)
            f.flush()
            os.fsync(f.fileno())
        os.replace(tmp, path)
    except BaseException:
        try:
            os.remove(tmp)
        except OSError:
            pass
        raise


def build(store, cfg, now, runs, gaps, mode, stop, stop_now, usage_pause_until,
          paused, hour_runs):
    """runs: active-run dicts already enriched with working/building."""
    tasks = store.tasks()
    by_id = {t["id"]: t for t in tasks}
    pools = {}
    for kind, pool in POOL_OF_KIND.items():
        mine = [r for r in runs if r["role"] == kind]
        pools[pool] = {"live": len(mine),
                       "working": sum(1 for r in mine if r["working"]),
                       "paused": pool in paused}
    for track, floor in sorted(cfg.floors.items()):
        mine = [r for r in runs if r["role"] == "engineer"
                and by_id.get(r["task_id"], {}).get("track") == track]
        pools[f"build:{track}"] = {
            "floor": floor, "live": len(mine),
            "working": sum(1 for r in mine if r["working"]),
            "reason": gaps.get(track), "paused": track in paused}
    blocked = [{"task": t["id"], "branch": t["branch"], "note": t["note"]}
               for t in tasks if t["state"] == "blocked"]
    feats = []
    for f in store.features():
        mine = [t for t in tasks if t["feature_id"] == f["id"]]
        feats.append({
            "id": f["id"], "track": f["track"], "state": f["state"],
            "pr": f["pr"], "fixes": f["fixes"], "tasks_done": sum(
                1 for t in mine if t["state"] in DONE_STATES),
            "tasks_total": len(mine),
            "strip": [[t["id"], t["state"][:1], t["eng_name"]] for t in mine]})
    return {
        "updated": now, "mode": mode,
        "kill_switch": "STOP.NOW" if stop_now else "STOP" if stop else None,
        "usage_pause_until": usage_pause_until or None,
        "cap": cfg.cap, "live": len(runs), "paused": sorted(paused),
        "runs_last_hour": hour_runs,
        "open_prs": len({t["feature_id"] for t in tasks
                         if t["state"] in ("pr_open", "ci_fix")}),
        "max_open_prs": cfg.max_open_prs,
        "ready_unpr": sum(1 for t in tasks if t["state"] == "integrating"),
        "max_ready_unpr": cfg.max_ready_unpr,
        "pools": pools,
        "runs": [{"id": r["id"], "task": r["task_id"], "role": r["role"],
                  "name": r["name"], "pid": r["pid"], "started": r["started"],
                  "last_beat": r["last_beat"], "working": r["working"],
                  "building": r["building"], "suspect": bool(r["suspect"])}
                 for r in runs],
        "tasks": [{"id": t["id"], "state": t["state"], "track": t["track"],
                   "kind": t["kind"], "attempts": t["attempts"],
                   "pr": t["pr"], "branch": t["branch"], "age": now
                   - t["created"]} for t in tasks],
        "features": feats,
        "blocked": blocked,
        "waits_on_owner": blocked,
        "privileged_pending": [t["id"] for t in tasks
                               if "needs_privileged" in (t["brief"] or "")
                               and t["state"] == "qa"],
        "reservations": store.reservations(),
    }
