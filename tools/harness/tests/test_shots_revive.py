"""A feature blocked on failed screenshots goes back to integrating while
Docker is up, a bounded number of times (automation.write, blocked for days
with its branch merging cleanly, 2026-11-13)."""
import support
from harness import shots_revive, triage_sweep

FID = "auto.write"


class Docker:
    def __init__(self, up=True):
        self.up = up

    def held(self, now):
        return frozenset() if self.up else frozenset({"build"})


class ShotsRevive(support.DaemonCase):
    def feature(self, o, integrated=1):
        s = o.store
        s.add_feature(FID, "daw", branch=f"feat/{FID}")
        for n in ("adr", "ui"):
            s.add_task(f"{FID}.{n}", "daw", "ui-engineer", state="blocked",
                       feature_id=FID)
            s.set_task(f"{FID}.{n}", integrated=integrated,
                       note="screenshot regeneration failed:\nlog tail")
        s.set_feature(FID, state="blocked")
        s.set_meta(f"shots:{FID}", '{"state": "failed", "tries": 2}')

    def states(self, o):
        return {t["state"] for t in o.store.feature_tasks(FID)}

    def test_revived_once_per_sweep_up_to_the_bound(self):
        o = self.make(dry_run=True, floors={"daw": 0})
        o.docker = Docker()
        self.feature(o)
        for n in range(shots_revive.MAX_REVIVES):
            triage_sweep.sweep(o, o.clock())
            self.assertEqual(self.states(o), {"integrating"})
            self.assertEqual(o.store.get_feature(FID)["state"], "integrating")
            self.assertEqual(o.store.get_meta(f"shots:{FID}"), "{}")
            o._block_feature(o.store.get_feature(FID),
                             "screenshot regeneration failed:\nagain")
        triage_sweep.sweep(o, o.clock())
        self.assertEqual(self.states(o), {"blocked"})

    def test_not_while_docker_is_down(self):
        o = self.make(dry_run=True, floors={"daw": 0})
        o.docker = Docker(up=False)
        self.feature(o)
        triage_sweep.sweep(o, o.clock())
        self.assertEqual(self.states(o), {"blocked"})
        self.assertIsNone(o.store.get_meta(shots_revive.KEY + FID))

    def test_not_when_a_slice_was_never_integrated(self):
        o = self.make(dry_run=True, floors={"daw": 0})
        o.docker = Docker()
        self.feature(o, integrated=0)
        triage_sweep.sweep(o, o.clock())
        self.assertEqual(self.states(o), {"blocked"})
