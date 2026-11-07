"""Number reservation seeded from origin/main and open PR diffs, and the
GhForge command shapes (ADR-0047 4a step 1)."""
import json
import unittest

import support
from harness import numbers
from harness.forge import ForgeError, GhForge

HDR = "contracts/api/include/sonora/api/messages.h"
FMT = "common/model/include/sonora/model/session_file.h"
WIN = "common/model/src/op_codec_detail.h"


def forge_with_state():
    f = support.FakeForge()
    f.main_paths = ["brain/decisions/ADR-0045-a.md",
                    "brain/decisions/ADR-0046-b.md", "README.md"]
    f.main = {HDR: "constexpr uint32_t kProtocolVersion = 42;",
              FMT: "constexpr std::int64_t kFormatVersion = 18;",
              WIN: "constexpr std::int64_t kWindowFormat = 15;"}
    return f


class SeedTests(unittest.TestCase):
    def test_adr_from_main_and_open_pr_files(self):
        f = forge_with_state()
        self.assertEqual(numbers.seed("adr", f), 46)
        f.prs[9] = {"branch": "x", "files": ["brain/decisions/ADR-0049-c.md",
                                             "src/ADR-0090.cpp"],
                    "state": "open", "mergeable": "yes", "checks": {}}
        self.assertEqual(numbers.seed("adr", f), 49)
        self.assertEqual(numbers.seed("adr", f, configured=60), 60)

    def test_versions_from_main_headers(self):
        f = forge_with_state()
        self.assertEqual(numbers.seed("protocol", f), 42)
        self.assertEqual(numbers.seed("format", f), 18)
        self.assertEqual(numbers.seed("window", f), 15)

    def test_versions_from_open_pr_diffs(self):
        f = forge_with_state()
        f.prs[3] = {"branch": "b", "files": [HDR, WIN], "state": "open",
                    "mergeable": "yes", "checks": {}}
        f.diffs[3] = ("-constexpr uint32_t kProtocolVersion = 42;\n"
                      "+constexpr uint32_t kProtocolVersion = 44;\n"
                      "+constexpr std::int64_t kWindowFormat = 16;\n")
        f.prs[4] = {"branch": "c", "files": ["other.cpp"], "state": "open",
                    "mergeable": "yes", "checks": {}}
        f.diffs[4] = "+constexpr uint32_t kProtocolVersion = 99;\n"
        self.assertEqual(numbers.seed("protocol", f), 44)  # 4 is unrelated
        self.assertEqual(numbers.seed("window", f), 16)
        self.assertEqual(numbers.seed("format", f), 18)

    def test_a_failed_read_raises_instead_of_guessing(self):
        for what in ("open_prs", "main_files"):
            f = forge_with_state()
            f.fail.add(what)
            with self.assertRaises(ForgeError):
                numbers.seed("adr", f)
        f = forge_with_state()
        f.fail.add("main_text")
        with self.assertRaises(ForgeError):
            numbers.seed("protocol", f)


class ReservationTests(support.DaemonCase):
    def test_two_features_never_get_the_same_number(self):
        o = self.make(seed_fn=None)
        f = self.forge
        f.main_paths = ["brain/decisions/ADR-0046-b.md"]
        f.main = {HDR: "kProtocolVersion = 42;", FMT: "kFormatVersion = 18;",
                  WIN: "kWindowFormat = 15;"}
        wants = ["adr", "protocol", "format", "window"]
        for tid in ("x", "y"):
            o.store.add_task(tid, "daw", "rt-engineer", wants=wants)
        for tid in ("x", "y"):
            o._reserve_numbers(o.store.get_task(tid))
        got = {}
        for r in o.store.reservations():
            got.setdefault(r["kind"], []).append(r["number"])
        self.assertEqual(got, {"adr": [47, 48], "protocol": [43, 44],
                               "format": [19, 20], "window": [16, 17]})
        t = o.store.get_task("y")
        self.assertEqual((t["adr_no"], t["protocol_no"]), (48, 44))

    def test_unreadable_repo_defers_the_launch(self):
        o = self.make(seed_fn=None)
        self.forge.fail.add("open_prs")
        o.store.add_task("x", "daw", "rt-engineer", wants=["adr"])
        o.tick()
        self.assertEqual(o.store.runs_of("x"), [])
        self.assertEqual(o.store.reservations(), [])
        self.assertEqual(len(o.store.events("x", "reserve_failed")), 1)
        self.assertGreater(o.store.get_task("x")["not_before"],
                           self.clock())


class GhFormTests(unittest.TestCase):
    """The argv GhForge builds; no process is started."""

    class R:
        def __init__(self, out="", rc=0, err=""):
            self.stdout, self.returncode, self.stderr = out, rc, err

    def forge(self, replies):
        calls = []

        def runner(argv, cwd=None, timeout=0):
            calls.append(argv)
            return replies.pop(0)
        return GhForge("/repo", runner=runner), calls

    def test_open_prs_and_create_and_status(self):
        f, calls = self.forge([
            self.R(json.dumps([{"number": 5, "headRefName": "feat/a",
                                "files": [{"path": "x.cpp"}]}])),
            self.R("https://github.com/o/r/pull/77\n"),
            self.R(json.dumps({"state": "OPEN", "mergeable": "CONFLICTING"})),
            self.R(json.dumps([{"name": "build", "bucket": "fail"},
                               {"name": "lint", "bucket": "pass"},
                               {"name": "asan", "bucket": "pending"}]), 1)])
        self.assertEqual(f.open_prs(), [{"number": 5, "branch": "feat/a",
                                         "files": ["x.cpp"]}])
        self.assertEqual(f.create_pr("feat/a", "main", "t", "b"), 77)
        create = calls[1]
        self.assertEqual(create[:3], ["gh", "pr", "create"])
        self.assertEqual(create[create.index("--base") + 1], "main")
        self.assertIn("--body-file", create)
        st = f.pr_status(77)
        self.assertEqual(st["mergeable"], "no")
        self.assertEqual([(c["name"], c["state"]) for c in st["checks"]],
                         [("build", "fail"), ("lint", "pass"),
                          ("asan", "pending")])

    def test_failing_command_raises(self):
        f, _ = self.forge([self.R("", 1, "boom")])
        with self.assertRaises(ForgeError):
            f.open_prs()


if __name__ == "__main__":
    unittest.main()
