"""Disk space for the dashboard: the host drive that holds the repo (the disk
filling up killed the daemon on 2026-11-06), Docker's own usage and the
biggest consumers inside the repo. The slow sources run in background
threads (dashboard_bg) so the page never waits on them."""
import json
import re
import shutil

from dashboard_bg import Periodic
from dashboard_diskscan import scan_repo
from dashboard_util import main_root, run

WARN_FREE = 30 * (1024 ** 3)
CRIT_FREE = 10 * (1024 ** 3)
DOCKER_EVERY_S = 60
SCAN_EVERY_S = 300

_UNITS = {"B": 1, "KB": 1000, "MB": 1000 ** 2, "GB": 1000 ** 3, "TB": 1000 ** 4}


def level(free_bytes):
    """ok, warn (under 30 GiB free) or crit (under 10 GiB free)."""
    if free_bytes < CRIT_FREE:
        return "crit"
    return "warn" if free_bytes < WARN_FREE else "ok"


def read_drive(path):
    u = shutil.disk_usage(path)
    return {"ok": True, "path": path, "total": u.total, "free": u.free, "used": u.used, "level": level(u.free)}


def parse_size(text):
    """Docker prints sizes in decimal units ("12.5GB", "0B", "3.2kB"); -1 when unreadable."""
    m = re.match(r"^\s*([\d.]+)\s*([A-Za-z]+)", str(text or ""))
    unit = _UNITS.get(m.group(2).upper()) if m else None
    return int(float(m.group(1)) * unit) if unit else -1


def parse_docker_df(text):
    """Rows from `docker system df --format '{{json .}}'`; [] when it is not that."""
    rows = []
    for ln in (text or "").splitlines():
        try:
            d = json.loads(ln)
        except ValueError:
            continue
        if not isinstance(d, dict) or "Type" not in d:
            continue
        rec = str(d.get("Reclaimable") or "")
        rows.append({"type": d["Type"], "count": int(d.get("TotalCount") or 0), "active": int(d.get("Active") or 0),
                     "size": parse_size(d.get("Size")), "reclaimable": parse_size(rec.split(" ")[0])})
    return rows


def read_docker():
    rows = parse_docker_df(run(["docker", "system", "df", "--format", "{{json .}}"], timeout=45))
    return {"ok": True, "rows": rows} if rows else None


_docker = Periodic(read_docker, DOCKER_EVERY_S)
_scan = Periodic(lambda: scan_repo(main_root()), SCAN_EVERY_S)


def disk_view():
    """The page's disk section; each part degrades on its own."""
    try:
        drive = read_drive(main_root())
    except OSError as e:
        drive = {"ok": False, "error": str(e)}
    docker = _docker.value()
    if docker is None:
        docker = {"ok": False, "error": "lendo..."}
    elif not docker.get("ok"):
        docker = {"ok": False, "error": "docker indisponível"}
    repo = _scan.value() or {"ok": False, "error": "medindo..."}
    return {"drive": drive, "docker": dict(docker, age=_docker.age()), "repo": dict(repo, age=_scan.age())}
