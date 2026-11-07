"""Bundles of the merge train (owner, 2026-11-04: PRs of one context ship
as one PR). Split out of train.py, which Train mixes this into; the methods
use Train's state helpers (info, _save) and its orchestrator (self.o)."""
from .forge import ForgeError


def bundle_agent(files):
    """Imported lazily: train.py owns the path -> agent table."""
    from .train import bundle_agent as pick
    return pick(files)


class BundleMixin:
    # -- bundles --------------------------------------------------------
    # Owner, 2026-11-04: PRs of one context ship as one PR (one CI run, one
    # merge). An integrator merges the members onto bundle/<name> from main;
    # the daemon pushes it, opens the one PR, closes the members that made it
    # in and puts the others back on the train alone.
    def is_bundle(self, fid):
        return bool((self.info(fid) or {}).get("bundle"))

    def bundle(self, name, numbers):
        fid, branch = f"bundle-{name}", f"bundle/{name}"
        if self.info(fid):
            raise ValueError(f"bundle {name} exists")
        members, files = [], set()
        for n in numbers:
            mf = f"pr-{n}"
            i = self.info(mf)
            if not i or i.get("bundled_into") or \
                    i.get("phase") in ("mergeado", "fechado"):
                raise ValueError(f"#{n} is not an open PR on the train")
            members.append({"pr": n, "branch": i["branch"],
                            "title": i.get("title") or ""})
            files |= set((self.o.store.get_task(mf) or {}).get("files") or [])
        track, agent = bundle_agent(files)
        with self.o.store.tx():
            self.o.store.add_task(
                fid, track, agent, state="ci_fix",
                branch=branch, files=sorted(files),
                brief=f"Bundle {name}: " + ", ".join(
                    f"#{m['pr']}" for m in members))
            self.o.store.set_task(fid, fix_kind="bundle",
                                  note="bundle: merge the member PRs")
            self.o.store.set_feature(fid, branch=branch, state="ci_fix")
        self._save(fid, bundle=members, branch=branch, base="main",
                   title=f"bundle {name}", phase="juntando",
                   next="integrator junta " + ", ".join(
                       f"#{m['pr']}" for m in members),
                   action="bundle")
        for m in members:
            self._member_to_pr_open(f"pr-{m['pr']}")
            self._save(f"pr-{m['pr']}", bundled_into=fid, phase="no bundle",
                       next=f"junto em {name}")
        self.o.store.event(fid, "bundle", " ".join(
            f"#{m['pr']}" for m in members))
        return fid

    def _member_to_pr_open(self, mf):
        """A member waiting for a fixer or integrator does not get one: the
        bundle integrator merges main for all of them."""
        t = self.o.store.get_task(mf)
        if t and t["state"] in ("ci_fix", "blocked"):
            self.o.store.advance(mf, "pr_open", note="bundled")
            self.o.store.set_task(mf, fix_kind=None)
            self.o.store.set_feature(mf, state="pr_open")

    def bundle_brief(self, fid):
        i = self.info(fid) or {}
        lines = "; ".join(f"#{m['pr']} {m['branch']}" for m in i["bundle"])
        return (
            "\nBundle run (owner: PRs of one context ship as one PR). You are "
            "on %s, which starts at origin/main. Merge these PRs in this "
            "order: %s. For each: `git fetch origin <branch>` then `git merge "
            "--no-ff origin/<branch>`; resolve conflicts keeping both sides' "
            "intent; when two PRs took the same ADR, protocol or format "
            "number, renumber the later one to the next free one. A PR you "
            "cannot merge sensibly: `git merge --abort`, leave it out and "
            "name it in findings with the reason (it stays a PR of its own). "
            "Then tools/pre-pr.sh and fix what it lists; build and run the "
            "tests the merged areas touch once. Commit; never push, open or "
            "close a PR: the daemon does. result.json summary: one line per "
            "PR you merged and what you had to resolve." % (i["branch"],
                                                              lines))

    def open_bundle(self, fid, worktree, data=None):
        """The bundle integrator committed: push, open the one PR, close the
        members it contains. Returns the new PR number."""
        o, i = self.o, self.info(fid)
        name = fid[len("bundle-"):]
        inside = [m for m in i["bundle"]
                  if o.forge.contains(worktree, m["branch"])]
        if not inside:
            raise ForgeError(f"bundle {name} contains no member PR")
        o.forge.push_head(worktree, i["branch"])
        data = data or {}
        body = ["Bundle of PRs of one context: one CI run and one merge "
                "instead of %d (owner, 2026-11-04)." % len(inside), "",
                "## Included"]
        body += [f"- #{m['pr']} {m['title']}" for m in inside]
        if data.get("summary"):
            body += ["", "## Integrator", data["summary"]]
        for head, key in (("Follow-ups", "followups"),
                          ("Unverified", "unverified")):
            items = data.get(key) or []
            if items:
                body += ["", f"## {head}"] + [f"- {x}" for x in items]
        body += ["", "Each original PR is closed with a link here.", "",
                 getattr(o, "attribution", "")]
        title = (f"Bundle {name}: " + ", ".join(
            f"#{m['pr']}" for m in inside))[:100]
        n = o.forge.create_pr(i["branch"], "main", title, "\n".join(body))
        o.store.set_task(fid, pr=n)
        o.store.set_feature(fid, pr=n)
        for m in inside:
            mf = f"pr-{m['pr']}"
            o.forge.close_pr(m["pr"], f"Joined into #{n} (bundle {name}): "
                                      "one CI run and one merge for the "
                                      "whole context.")
            self._member_to_pr_open(mf)
            o.store.advance_feature(mf, "merged")
            self._save(mf, phase="no bundle", next=f"fechado → #{n}")
        for m in i["bundle"]:
            if m not in inside:
                self._save(f"pr-{m['pr']}", bundled_into=None,
                           phase="adotado", next="fora do bundle: segue só")
        self._save(fid, pr=n, bundle=inside, phase="CI rodando",
                   action=f"bundle aberto #{n}", next="CI do bundle")
        o.store.event(fid, "bundle_opened", f"#{n}")
        return n
