"""Merge train for PRs the daemon did not open (owner, 2026-11-04: "the
orchestrator drives every open PR to merge").

An adopted PR is a one-task feature in state pr_open whose branch is the PR's
head. Each CI poll takes the cheapest step that can move it:

  stacked on another open PR    wait; once that base is gone, retarget to main
  conflicts with main           a fixer (merge main, resolve), via _red
  checks pending                wait
  every check green             squash-merge, pinned to the head that passed
  red or cancelled, first time  no agent: rerun the failed jobs, or merge main
                                into the branch so the run sees today's main
  still red on a refreshed head a fixer for the failing checks, via _red

A refresh starts a full CI run, so at most `train_slots` PRs run CI at once;
the rest wait for a slot. GitHub rate limits pause every gh call.
"""
import json

from .forge import ForgeError, RateLimited

META = "train"
RATE_PAUSE_S = 600
LIST_TTL_S = 300


def track_of(files):
    if any(f.startswith(("iso/", "audio-stack/")) for f in files):
        return "os"
    if any(f.startswith("plugins/") for f in files):
        return "plugin"
    return "daw"


def agent_of(track, files):
    if track == "os":
        return "distro-engineer"
    if track == "plugin":
        native = any(f.startswith(("plugins/native/", "common/dsp/"))
                     for f in files)
        return "dsp-engineer" if native else "plugin-engineer"
    code = [f for f in files if f.endswith((".cpp", ".h", ".hpp", ".qml"))]
    if code and all(f.startswith("ui/") for f in code):
        return "ui-engineer"
    return "rt-engineer"


class Train:
    def __init__(self, orch):
        self.o = orch
        self.pause_until = 0.0
        self._list = (0.0, [])

    # -- state ----------------------------------------------------------
    def _all(self):
        try:
            return json.loads(self.o.store.get_meta(META, "{}"))
        except ValueError:
            return {}

    def info(self, fid):
        return self._all().get(fid)

    def _save(self, fid, **fields):
        d = self._all()
        cur = d.setdefault(fid, {})
        cur.update(fields)
        cur["updated"] = self.o.clock()
        self.o.store.set_meta(META, json.dumps(d))
        return cur

    def is_adopted(self, fid):
        return fid in self._all()

    def rows(self):
        """For status.json: one row per adopted PR, what the daemon is doing
        about it and what comes next."""
        out = []
        for fid, i in sorted(self._all().items(),
                             key=lambda kv: kv[1].get("pr") or 0):
            f = self.o.store.get_feature(fid) or {}
            t = (self.o.store.feature_tasks(fid) or [{}])[0]
            run = next((r for r in self.o.store.active_runs()
                        if r["task_id"] == t.get("id")), None)
            out.append({
                "pr": i.get("pr"), "title": i.get("title"),
                "branch": i.get("branch"), "base": i.get("base"),
                "track": f.get("track"), "agent": t.get("agent"),
                "state": f.get("state"), "phase": i.get("phase"),
                "next": i.get("next"), "last_action": i.get("action"),
                "waits_on": i.get("waits_on"), "fixes": f.get("fixes", 0),
                "fixer": run["name"] if run else None,
                "passed": i.get("passed"), "total": i.get("total"),
                "failing": i.get("failing") or [],
                "updated": i.get("updated")})
        return out

    # -- adoption -------------------------------------------------------
    def adopt(self, numbers=None):
        """Adopt open PRs (all, or the given numbers) that no feature owns.
        Returns the adopted PR numbers."""
        owned = {f["pr"] for f in self.o.store.features() if f["pr"]}
        done = []
        for p in self.o.forge.list_prs():
            n = p["number"]
            if n in owned or (numbers and n not in numbers):
                continue
            fid = f"pr-{n}"
            track = track_of(p["files"])
            agent = agent_of(track, p["files"])
            with self.o.store.tx():
                self.o.store.add_task(
                    fid, track, agent, state="pr_open", branch=p["branch"],
                    files=p["files"], brief=p.get("title", ""))
                self.o.store.set_task(fid, pr=n)
                self.o.store.set_feature(fid, pr=n, branch=p["branch"],
                                         state="pr_open")
            self._save(fid, pr=n, title=p.get("title"), branch=p["branch"],
                       base=p.get("base"), phase="adotado",
                       next="ler o CI", action="adopted")
            self.o.store.event(fid, "adopted_pr", f"#{n} {p['branch']}")
            done.append(n)
        return done

    # -- one poll -------------------------------------------------------
    def _open_list(self, now):
        at, rows = self._list
        if now - at > LIST_TTL_S:
            rows = self.o.forge.list_prs()
            self._list = (now, rows)
        return rows

    def busy(self):
        """PRs whose CI the train set running and which have not finished."""
        return sum(1 for i in self._all().values()
                   if i.get("phase") in ("CI rodando", "atualizado",
                                         "rerodando"))

    def step(self, f, ts, now):
        """Returns False when GitHub is rate limiting (the caller stops
        polling for this tick)."""
        if now < self.pause_until:
            return False
        try:
            self._step(f, ts, now)
        except RateLimited as e:
            self.pause_until = now + RATE_PAUSE_S
            self.o.store.event(ts[0]["id"], "rate_limited", str(e))
            self._save(f["id"], next=f"GitHub rate limit: pausa até "
                                     f"+{RATE_PAUSE_S // 60} min")
            return False
        except ForgeError as e:
            self.o.store.event(ts[0]["id"], "train_error", str(e))
            self._save(f["id"], next=f"erro do gh: {str(e)[:120]}")
        return True

    def _step(self, f, ts, now):
        o, fid, n = self.o, f["id"], f["pr"]
        cfg = o.cfg
        i = self.info(fid) or {}
        d = o.forge.pr_detail(n)
        if d["state"] == "merged":
            self._save(fid, phase="mergeado", next=None, action="merged")
            o._merged(f, ts)
            return
        if d["state"] == "closed":
            self._save(fid, phase="fechado", next="decisão do owner")
            o._block_feature(f, f"PR #{n} closed without merge")
            return
        head = d["head_sha"]
        upd = {"base": d["base"], "head": head}
        # The first new head after a refresh or a fixer push is the one
        # whose CI decides; it is never refreshed again.
        if i.get("refresh_from") and head != i["refresh_from"]:
            upd.update(refreshed=head, refresh_from=None)
            i["refreshed"] = head
        checks = d["checks"]
        passed = sum(1 for c in checks if c["state"] == "pass")
        upd.update(passed=passed, total=len(checks), waits_on=None,
                   failing=[c["name"] for c in checks
                            if c["state"] in ("fail", "cancel")])

        if d["base"] and d["base"] != "main":
            owner = next((p["number"] for p in self._open_list(now)
                          if p["branch"] == d["base"]), None)
            if owner:
                self._save(fid, phase="empilhado", waits_on=owner,
                           next=f"aguarda o merge de #{owner}",
                           **{k: v for k, v in upd.items()
                              if k != "waits_on"})
                return
            o.forge.retarget(n, "main")
            self._save(fid, phase="retarget", action="retarget → main",
                       next="atualizar com a main", **upd)
            return

        if d["mergeable"] == "no":
            self._save(fid, phase="conflito", action="fixer (conflito)",
                       next="fixer faz merge da main e resolve", **upd)
            o._red(f, ts, ["merge-conflict"], "conflict")
            return

        if any(c["state"] == "pending" for c in checks):
            self._save(fid, phase="CI rodando",
                       next=f"CI {passed}/{len(checks)}", **upd)
            return

        fails = [c["name"] for c in checks if c["state"] == "fail"]
        cancels = [c["name"] for c in checks if c["state"] == "cancel"]
        if checks and not fails and not cancels:
            if d["mergeable"] != "yes":
                self._save(fid, phase="verde", next="GitHub calculando "
                           "mergeable", **upd)
                return
            if o._is_dry("merge"):
                self._save(fid, phase="verde", next="merge (pool merge em "
                           "dry-run)", **upd)
                return
            o.forge.merge(n, head)
            self._save(fid, phase="mergeado", action=f"merge {head[:8]}",
                       next=None, **upd)
            o._merged(f, ts)
            return

        refreshed = i.get("refreshed") == head
        if not fails and refreshed and i.get("reruns_head") == head \
                and i.get("reruns", 0) >= 2:
            self._save(fid, phase="bloqueado", next="cancelado 3x no mesmo "
                       "head: decisão do owner", **upd)
            o._block_feature(f, f"PR #{n}: checks cancelled three times")
            return
        if fails and refreshed:
            self._save(fid, phase="fixer", action="fixer (CI)",
                       next="fixer corrige: " + ", ".join(fails[:3]), **upd)
            o._red(f, ts, fails, "ci")
            return
        if self.busy() >= cfg.train_slots:
            self._save(fid, phase="aguardando vaga",
                       next=f"vaga no CI ({cfg.train_slots} por vez)", **upd)
            return
        # Cheapest refresh: a cancelled run on a current head is rerun; a
        # red one gets today's main first (most reds were fixed on main).
        how = "rerun"
        if fails or not checks:
            r = o.forge.update_branch(n)
            if r == "conflict":
                self._save(fid, phase="conflito", action="update-branch: "
                           "conflito", next="fixer faz merge da main", **upd)
                o._red(f, ts, ["merge-conflict"], "conflict")
                return
            how = "update-branch" if r == "ok" else "rerun"
        if how == "rerun":
            if not o.forge.rerun_failed(n, head):
                o.forge.update_branch(n)
                how = "update-branch (sem run para rerodar)"
            upd["refreshed"] = head
            upd["reruns"] = (i.get("reruns", 0) + 1
                             if i.get("reruns_head") == head else 1)
            upd["reruns_head"] = head
            phase = "rerodando"
        else:
            upd["refresh_from"] = head
            phase = "atualizado"
        self._save(fid, phase=phase, action=how,
                   next="novo CI; vermelho de novo → fixer", **upd)
        o.store.event(ts[0]["id"], "train_refresh", f"#{n} {how}")

    def after_fix(self, fid, local_head):
        """The fixer's commits are pushed: the next head is the one to judge,
        without another refresh. A fixer that committed nothing found the
        cause outside the PR (runner, network, already fixed on main): its
        failed jobs are rerun once instead."""
        i = self.info(fid) or {}
        head_before = i.get("head")
        if local_head and local_head == head_before:
            self.o.forge.rerun_failed(i.get("pr"), head_before)
            self._save(fid, phase="rerodando", action="fixer: causa externa,"
                       " rerun", refreshed=head_before,
                       next="rerun; vermelho de novo → fixer")
            return
        self._save(fid, phase="CI rodando", action="push do fixer",
                   refresh_from=head_before, next="CI do fix")
