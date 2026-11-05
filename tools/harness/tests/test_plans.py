"""plan.json and assignments.json validators (ADR-0047 4a)."""
import copy
import unittest

import support  # noqa: F401  (puts tools/ on sys.path)
from harness import plans


def task(i, kind, files, deps=(), agent="rt-engineer"):
    return {"id": i, "kind": kind, "title": i, "brief": "b",
            "files": list(files), "depends_on": list(deps), "agent": agent,
            "accept": ["true"]}


def good():
    return {
        "version": 1, "feature": "f", "backlog_ids": ["f.item"],
        "track": "daw", "feature_branch": "feat/f",
        "reserved": {"adr": 47, "protocol": None, "formats": []},
        "adr": "brain/decisions/ADR-0047-f.md",
        "tasks": [task("c", "contract", ["contracts/f/**"]),
                  task("e", "engine", ["engine/f/**"], ["c"]),
                  task("u", "ui", ["ui/f/**"], ["e"], "ui-engineer"),
                  task("d", "docs", ["docs/f/**"], ["u"])],
        "merge_order": ["c", "e", "u", "d"],
        "pr": {"title": "t", "partial": False, "partial_reason": None}}


EXPECTED = {"feature": "f", "track": "daw", "adr": 47, "protocol": None,
            "formats": []}


def errors(plan, **kw):
    return plans.validate_plan(plan, kw.get("expected", EXPECTED),
                               kw.get("active"))


def has(errs, text):
    return any(text in e for e in errs)


class PlanValidator(unittest.TestCase):
    def test_a_good_plan_is_valid(self):
        self.assertEqual(errors(good()), [])

    def test_schema_shape(self):
        p = good()
        del p["merge_order"]
        p["tasks"][0]["kind"] = "magic"
        p["extra"] = 1
        e = errors(p)
        self.assertTrue(has(e, "missing merge_order"))
        self.assertTrue(has(e, "unknown field extra"))
        self.assertTrue(has(e, "'magic' not one of"))

    def test_file_overlap_between_tasks(self):
        p = good()
        p["tasks"][2]["files"] = ["engine/f/ui/**"]   # nests under engine/f
        self.assertTrue(has(errors(p), "files of e and u overlap"))

    def test_file_overlap_with_an_active_feature(self):
        e = errors(good(), active={"other": ["engine/f/x.cpp"]})
        self.assertTrue(has(e, "e overlap active feature other"))
        self.assertEqual(errors(good(), active={"other": ["docs2/**"]}), [])

    def test_contract_first(self):
        p = good()
        p["tasks"][1]["depends_on"] = []     # engine no longer needs it
        self.assertTrue(has(errors(p), "must depend on contract task c"))
        p = good()
        p["merge_order"] = ["e", "c", "u", "d"]
        e = errors(p)
        self.assertTrue(has(e, "before its dependency c"))
        self.assertTrue(has(e, "before contract task c"))

    def test_contract_has_no_dependencies(self):
        p = good()
        p["tasks"].append(task("c2", "contract", ["contracts/g/**"]))
        p["merge_order"].append("c2")
        p["tasks"][0]["depends_on"] = ["c2"]
        self.assertTrue(has(errors(p), "a contract task has no dependencies"))

    def test_kind_order(self):
        p = good()
        p["tasks"][2]["depends_on"] = ["c"]
        p["tasks"][1]["depends_on"] = ["c", "u"]   # engine after ui
        self.assertTrue(has(errors(p), "contract, engine, ui, tests, docs"))

    def test_depends_on_must_be_acyclic(self):
        p = good()
        p["tasks"][1]["kind"] = p["tasks"][2]["kind"] = "engine"
        p["tasks"][1]["depends_on"] = ["c", "u"]
        e = errors(p)
        self.assertTrue(has(e, "cycle"), e)

    def test_depends_on_unknown_and_self(self):
        p = good()
        p["tasks"][1]["depends_on"] = ["c", "zz", "e"]
        e = errors(p)
        self.assertTrue(has(e, "unknown task zz"))
        self.assertTrue(has(e, "depends on itself"))

    def test_merge_order_is_a_permutation(self):
        p = good()
        p["merge_order"] = ["c", "e", "u"]
        self.assertTrue(has(errors(p), "every task exactly once"))

    def test_duplicate_ids(self):
        p = good()
        p["tasks"][3]["id"] = "u"
        self.assertTrue(has(errors(p), "duplicate task id u"))

    def test_reserved_numbers(self):
        p = good()
        p["reserved"]["adr"] = 48            # the architect picked one
        self.assertTrue(has(errors(p), "reserved.adr is 48"))
        p = good()
        p["reserved"]["protocol"] = 9        # none was reserved
        self.assertTrue(has(errors(p), "reserved.protocol is 9"))
        p = good()
        p["reserved"]["formats"] = [20]
        self.assertTrue(has(errors(p), "formats [20]"))

    def test_adr_file_must_carry_the_reserved_number(self):
        p = good()
        p["adr"] = "brain/decisions/ADR-0046-f.md"
        self.assertTrue(has(errors(p), "must contain ADR-0047"))
        p = good()
        p["adr"] = None
        self.assertTrue(has(errors(p), "adr (the file) is null"))
        p = good()
        p["reserved"]["adr"] = None
        self.assertTrue(has(errors(p), "no ADR number was reserved"))

    def test_no_adr_needed_is_fine(self):
        p = good()
        p["reserved"]["adr"] = None
        p["adr"] = None
        self.assertEqual(errors(p), [])

    def test_feature_track_and_branch_match_the_request(self):
        p = good()
        p["track"] = "os"
        p["feature_branch"] = "feat/g"
        e = errors(p)
        self.assertTrue(has(e, "track is 'os'"))
        self.assertTrue(has(e, "feature_branch must be feat/f"))

    def test_partial_needs_a_reason(self):
        p = good()
        p["pr"]["partial"] = True
        self.assertTrue(has(errors(p), "partial_reason"))


def table(plan=None):
    plan = plan or good()
    rows = []
    for i, t in enumerate(plan["tasks"]):
        rows.append({"task": t["id"], "engineer": t["agent"],
                     "tester": "tester",
                     "sub_branch": plans.sub_branch("f", t["id"]),
                     "order": i + 1,
                     "start_from": plans.sub_branch("f", t["depends_on"][0])
                     if t["depends_on"] else "feat/f"})
    return {"version": 1, "feature": "f", "rows": rows}


class AssignmentValidator(unittest.TestCase):
    def check(self, t):
        return plans.validate_assignments(t, good())

    def test_a_good_table_is_valid(self):
        self.assertEqual(self.check(table()), [])

    def test_every_task_has_exactly_one_row(self):
        t = table()
        t["rows"].pop()
        self.assertTrue(has(self.check(t), "no row for task d"))
        t = table()
        t["rows"].append(copy.deepcopy(t["rows"][0]))
        self.assertTrue(has(self.check(t), "two rows for task c"))
        t = table()
        t["rows"][0]["task"] = "zz"
        self.assertTrue(has(self.check(t), "unknown task zz"))

    def test_dependency_order(self):
        t = table()
        t["rows"][1]["order"], t["rows"][0]["order"] = 1, 2
        self.assertTrue(has(self.check(t), "after its dependency c"))

    def test_engineer_must_fit_the_track(self):
        t = table()
        t["rows"][1]["engineer"] = "distro-engineer"
        self.assertTrue(has(self.check(t), "not an engineer of track daw"))
        t["rows"][1]["engineer"] = "ui-engineer"     # a better fit is allowed
        self.assertEqual(self.check(t), [])

    def test_sub_branch_and_start_from_are_fixed(self):
        t = table()
        t["rows"][1]["sub_branch"] = "feat/f/e"
        t["rows"][2]["start_from"] = "feat/f"
        e = self.check(t)
        self.assertTrue(has(e, "sub_branch must be sub/f/e"))
        self.assertTrue(has(e, "start_from must be one of"))

    def test_sub_branches_never_nest_under_the_feature_branch(self):
        """git cannot hold refs feat/f and feat/f/t at once."""
        self.assertFalse(plans.sub_branch("f", "t").startswith("feat/f/"))

    def test_schema_and_feature(self):
        t = table()
        t["rows"][0]["tester"] = "someone"
        self.assertTrue(has(self.check(t), "must be 'tester'"))
        t = table()
        t["feature"] = "g"
        self.assertTrue(has(self.check(t), "the plan is 'f'"))


if __name__ == "__main__":
    unittest.main()
