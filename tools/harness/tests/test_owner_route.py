"""Only a real owner ruling waits on the owner (2026-11-13).

The live daemon listed 23 items in waits_on_owner and the owner could act on
six: two were the daemon's own "third bounce: owner decision" (testers'
reasons, a claim too narrow, a build over the command cap), four asked the
owner to restart Docker or re-queue an integrator, one said "Not an owner
decision" in so many words, and nine were hardware runs the owner parked
until the product is complete. Each of those also kept its feature out of
the stall resolver (stall_check.waits_elsewhere), so the feature sat silent.
"""
import json
import os
import unittest

import support
from harness import blockinfo, owner_infer, owner_route, stall_check

# Verbatim (trimmed) from the live state of 2026-11-13.
HW = {
    "crash": "Run tools/crash-recovery-hw.sh power-pull protocol (ADR-0102) "
             "on the UMC1820 hardware unit and report the result.",
    "kiosk": "Provide a KVM-capable host to run tools/test-boot-qemu.sh "
             "--gui and --gui-broken, or decide to flip the kiosk default "
             "without it.",
    "usb": "Write a physical USB stick with tools/write-usb.ps1 on Windows "
           "and report the result; item closes on that (ADR-0110).",
    "oop": "Host: run the bare-metal IPC measurement (RT kernel, UMC1820 18 "
           "ch, quantum 128) so the futex wake can be enabled or dropped.",
    "ampcab": "Decide which amps/cabs/IRs to capture (owned or rented "
              "hardware) and run the rig procedure.",
}
NOT_OWNER = {
    "docker": ("Owner/host action: restart Docker engine.", "infra"),
    "drain": ("Coordinator/owner: re-queue the integrator for "
              "plugins.host.editors.rest.wire (or reset its state to qa so "
              "it re-dispatches) and let the Docker build queue drain.",
              "infra"),
    "stems": ("Not an owner decision: default is to wait for "
              "ui.record.punch_loop to merge, then dispatch stems-ui.",
              "plan"),
    "protocol": ("Reserve the next protocol version (coordinator action) so "
                 "the layout-bump remainder can be planned, or decide to "
                 "drop it.", "plan"),
    "bounce": (owner_route.LEGACY_BOUNCE, "plan"),
}
REAL = {
    "licence": "Confirm libptformat (GPL) fits how Sonora is distributed.",
    "datasets": "Provide VocalSet and vocadito (CC BY 4.0) on the host, or "
                "approve downloading them.",
    "mixed": "Provide amp capture hardware/recordings for artist-tone "
             "matching, and decide whether a session key field is wanted.",
}


class Classify(unittest.TestCase):
    def test_hardware_runs_are_parked_not_the_owners(self):
        for k, ask in HW.items():
            self.assertEqual(owner_route.cause(ask), "hardware", k)

    def test_infra_and_plan_asks_are_not_the_owners(self):
        for k, (ask, want) in NOT_OWNER.items():
            self.assertEqual(owner_route.cause(ask), want, k)

    def test_a_real_ruling_stays_with_the_owner(self):
        for k, ask in REAL.items():
            self.assertIsNone(owner_route.cause(ask), k)
        self.assertIsNone(owner_route.cause(None))

    def test_the_sweep_does_not_mark_a_note_that_denies_the_owner(self):
        note = NOT_OWNER["stems"][0]
        self.assertIsNotNone(owner_infer.owner_from(note))  # the words match
        self.assertIsNone(owner_route.real_ask(owner_infer.owner_from(note),
                                               note))


class Daemon(support.DaemonCase):
    def make(self, **kw):
        kw.setdefault("floors", {"daw": 0})
        kw.setdefault("dry_run", True)
        return super().make(**kw)

    def blocked(self, o, tid, owner, note="n", fid=None, kind="work"):
        o.store.add_task(tid, "daw", "rt-engineer", state="blocked",
                         feature_id=fid, kind=kind)
        o.store.set_task(tid, owner=owner, note=note)

    def status(self, o):
        o.tick()
        with open(os.path.join(o.odir, "status.json")) as f:
            return json.load(f)

    def test_status_lists_only_real_rulings_and_parks_hardware(self):
        o = self.make()
        self.blocked(o, "hw", HW["crash"])
        self.blocked(o, "dock", NOT_OWNER["docker"][0])
        self.blocked(o, "bnc", owner_route.LEGACY_BOUNCE,
                     note="third bounce: owner decision")
        self.blocked(o, "lic", REAL["licence"])
        st = self.status(o)
        self.assertEqual([w["task"] for w in st["waits_on_owner"]], ["lic"])
        groups = {g["cause"]: [i["task"] for i in g["items"]]
                  for g in st["blocked"]}
        self.assertEqual(groups["hardware"], ["hw"])
        self.assertEqual(groups["infra"], ["dock"])
        self.assertEqual(groups["plan"], ["bnc"])
        label = next(g["label"] for g in st["blocked"]
                     if g["cause"] == "hardware")
        self.assertEqual(label, "validação em hardware, estacionado")

    def test_agent_hardware_ask_is_parked_with_the_ask_kept(self):
        o = self.make()
        o.store.add_task("w", None, "architect", kind="wake")
        o._on_agent_blocked(o.store.get_task("w"), "planner", {
            "summary": "Only the hardware power-pull run remains.",
            "needs_owner": HW["crash"]}, o.clock())
        t = o.store.get_task("w")
        self.assertEqual((t["state"], t["owner"], t["cause"]),
                         ("blocked", None, "hardware"))
        self.assertIn("UMC1820", t["note"])

    def test_agent_docker_ask_is_infra_not_the_owner(self):
        o = self.make()
        o.store.add_task("w", None, "principal-engineer", kind="wake")
        o._on_agent_blocked(o.store.get_task("w"), "planner", {
            "summary": "Docker Desktop died.",
            "needs_owner": NOT_OWNER["docker"][0]}, o.clock())
        t = o.store.get_task("w")
        self.assertEqual((t["owner"], t["cause"]), (None, "infra"))

    def test_a_false_owner_ask_no_longer_silences_the_feature(self):
        o = self.make()
        o.store.add_task("f.a", "daw", "rt-engineer", state="integrating",
                         feature_id="f")
        self.blocked(o, "wake-principal-stall_resolver-f-1",
                     NOT_OWNER["drain"][0], fid="f", kind="wake")
        self.assertFalse(stall_check.waits_elsewhere(
            o.store, o.store.get_task("f.a")))
        self.blocked(o, "f.b", REAL["licence"], fid="f")
        self.assertTrue(stall_check.waits_elsewhere(
            o.store, o.store.get_task("f.a")))


class ThirdBounce(support.DaemonCase):
    def make(self, **kw):
        kw.setdefault("floors", {"daw": 0})
        kw.setdefault("dry_run", True)
        return super().make(**kw)

    def test_third_bounce_goes_to_the_architect_with_the_reasons(self):
        o = self.make()
        o.store.add_feature("f", "daw", branch="feat/f")
        o.store.add_task("f.ui", "daw", "ui-engineer", state="qa",
                         feature_id="f", files=["ui/x/**"])
        o.store.set_task("f.ui", bounces=2)
        o._on_result({"task_id": "f.ui", "role": "tester"}, {
            "status": "fail",
            "summary": "live_loops_model.* is outside claimed globs",
            "findings": [{"severity": "blocking", "where": "ui/CMakeLists",
                          "text": "build exceeded the command cap"}]},
            o.clock())
        t = o.store.get_task("f.ui")
        self.assertEqual((t["state"], t["owner"], t["cause"], t["bounces"]),
                         ("blocked", None, "plan", 3))
        self.assertIn("third bounce", t["note"])
        self.assertIn("outside claimed globs", t["note"])
        (w,) = [w for w in o.store.tasks(kind="wake")
                if w["agent"] == "architect"]
        self.assertEqual((w["event"], w["feature_id"], w["state"]),
                         ("stall_resolver", "f", "queued"))
        for part in ("f.ui", "outside claimed globs", "command cap"):
            self.assertIn(part, w["brief"])
        self.assertEqual(blockinfo.owner_items(
            o.store.tasks(), {"f": o.store.get_feature("f")}), [])

    def test_a_second_third_bounce_does_not_queue_a_second_replan(self):
        o = self.make()
        o.store.add_feature("f", "daw", branch="feat/f")
        for tid in ("f.a", "f.b"):
            o.store.add_task(tid, "daw", "ui-engineer", state="qa",
                             feature_id="f", files=[f"{tid}/**"])
            o.store.set_task(tid, bounces=2)
            o._on_result({"task_id": tid, "role": "tester"},
                         {"status": "fail", "summary": "red"}, o.clock())
        self.assertEqual(len([w for w in o.store.tasks(kind="wake")
                              if w["agent"] == "architect"]), 1)


if __name__ == "__main__":
    unittest.main()
