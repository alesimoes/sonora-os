"""tools/local-ci.sh against a throwaway repository, with stubs for the lock
and the step scripts (a fake docker on PATH that always fails; no build
runs): which steps a change runs, the one rerun of a red step, the flakes
file, lock waits logged apart, and a run holding the lock for all its steps.
"""
import os
import shutil
import stat
import subprocess
import tempfile
import unittest

import support  # noqa: F401  (sets sys.path)
from harness import localci, localci_flakes, localci_steps

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))

# Logs every call (with whether the lock was already held), writes the
# configured wait, then runs the command holding the "lock".
STUB_SLOT = r'''#!/usr/bin/env bash
echo "held=${SONORA_HEAVY_HELD:-0} $*" >>"$STUB_LOG/slot.calls"
if [[ -n "${SONORA_HEAVY_WAIT_FILE:-}" && -z "${SONORA_HEAVY_HELD:-}" ]]; then
  echo "${STUB_WAIT:-0}" >"$SONORA_HEAVY_WAIT_FILE"
fi
unset SONORA_HEAVY_WAIT_FILE
export SONORA_HEAVY_HELD=1
exec "$@"
'''

# The ui step's script: red the first STUB_REDS times, with STUB_KIND's
# output (a ctest failure or a compile error), then green.
STUB_UI = r'''#!/usr/bin/env bash
n=$(( $(cat "$STUB_LOG/ui.runs" 2>/dev/null || echo 0) + 1 ))
echo "$n" >"$STUB_LOG/ui.runs"
if (( n <= ${STUB_REDS:-0} )); then
  if [[ "${STUB_KIND:-test}" == build ]]; then
    echo "ui/qt/src/x.cpp:12:3: error: no member named 'y'"
  else
    printf 'The following tests FAILED:\n\t 12 - session_saver (Failed)\n\t 40 - control (Timeout)\nErrors while running CTest\n'
  fi
  exit 8
fi
echo "100% tests passed"
'''


def _bash():
    exe = localci.bash_exe()
    return exe if shutil.which(exe) or os.path.exists(exe) else None


def _write(path, text, exe=False):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", newline="\n", encoding="utf-8") as f:
        f.write(text)
    if exe:
        os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR)


@unittest.skipUnless(_bash(), "needs bash")
class Script(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.repo = os.path.join(self.tmp, "repo").replace("\\", "/")
        shutil.copytree(os.path.join(REPO, "tools"),
                        os.path.join(self.repo, "tools"),
                        ignore=shutil.ignore_patterns("harness", "__pycache__"))
        _write(os.path.join(self.repo, "tools", "build-slot.sh"), STUB_SLOT, True)
        _write(os.path.join(self.repo, "tools", "build-ui.sh"), STUB_UI, True)
        _write(os.path.join(self.repo, "README.md"), "x\n")
        self.git("init", "-q", "-b", "main")
        self.commit("base")
        self.git("checkout", "-q", "-b", "pr")
        fake = os.path.join(self.tmp, "bin")
        _write(os.path.join(fake, "docker"), "#!/bin/sh\nexit 1\n", True)
        self.out = os.path.join(self.tmp, "out").replace("\\", "/")
        self.env = dict(os.environ, SONORA_LOCAL_CI_ROOT=self.repo,
                        SONORA_LOCAL_CI_DIR=self.out, STUB_LOG=self.out,
                        PATH=fake.replace("\\", "/") + os.pathsep
                        + os.environ["PATH"])
        for k in ("SONORA_HEAVY_HELD", "SONORA_LOCAL_CI_HOLD_LOCK",
                  "SONORA_LOCAL_CI_RERUN", "SONORA_LOCAL_CI_FAIL_FAST"):
            self.env.pop(k, None)

    def git(self, *args):
        subprocess.run(["git", "-c", "user.name=t", "-c", "user.email=t@t"]
                       + list(args), cwd=self.repo, check=True,
                       capture_output=True)

    def commit(self, msg):
        self.git("add", "-A")
        self.git("commit", "-qm", msg, "--allow-empty")

    def change(self, *paths):
        for p in paths:
            full = os.path.join(self.repo, p)
            old = ""
            if os.path.exists(full):
                with open(full, encoding="utf-8") as f:
                    old = f.read()
            _write(full, old + "# changed\n", p.endswith(".sh"))
        self.commit("change")

    def run_ci(self, *args, **env):
        e = dict(self.env, **{k: str(v) for k, v in env.items()})
        return subprocess.run(
            [localci.bash_exe(), self.repo + "/tools/local-ci.sh",
             "--base", "main"] + list(args),
            env=e, capture_output=True, text=True, cwd=self.repo, timeout=120)

    def steps_for(self, *paths):
        self.change(*paths)
        r = self.run_ci("--dry-run")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        if "nothing to build" in r.stdout:
            return []
        line = next(ln for ln in r.stdout.splitlines()
                    if ln.startswith("local-ci: ") and ":" not in ln[10:])
        return line[len("local-ci: "):].split()

    def read(self, name):
        with open(os.path.join(self.out, name), encoding="utf-8") as f:
            return f.read()

    # -- which steps a change runs ------------------------------------------
    def test_harness_dashboard_docs_and_the_gate_itself_build_nothing(self):
        self.assertEqual(self.steps_for(
            "tools/harness/x.py", "tools/orchestrator.py",
            "tools/dashboard.html", ".claude/agents/x.md", "docs/x.md",
            "planning/x.md", "brain/x.md", "tools/local-ci.sh",
            "tools/build-slot.sh"), [])

    def test_a_build_script_runs_only_the_steps_that_call_it(self):
        self.assertEqual(self.steps_for("tools/build-plugins.sh"),
                         ["plugins", "pluginhost"])

    def test_the_record_script_runs_the_record_step(self):
        self.assertEqual(self.steps_for("tools/test-engine-docker.sh"),
                         ["record"])

    def test_the_runtime_image_runs_the_steps_that_use_it(self):
        self.assertEqual(self.steps_for("tools/Dockerfile.runtime"),
                         ["record", "quantum", "bwf"])

    def test_an_engine_script_brings_the_tree_that_builds_its_engine(self):
        self.assertEqual(self.steps_for("tools/failed-take-quit.py"),
                         ["tree", "engine-scripts"])

    def test_an_unknown_build_script_still_runs_every_sonora_context(self):
        steps = self.steps_for("tools/build-something-new.sh")
        for s in ("tree", "engine-scripts", "plugins", "pluginhost", "tsan",
                  "rtsan", "record", "bwf", "windows"):
            self.assertIn(s, steps)

    def test_engine_code_is_unchanged(self):
        steps = self.steps_for("engine/core/x.cpp")
        for s in ("tree", "engine-scripts", "tsan", "rtsan", "record", "bwf"):
            self.assertIn(s, steps)

    # -- one rerun of a red step -------------------------------------------
    def test_a_step_green_on_its_rerun_passes_and_is_a_flake(self):
        self.change("ui/qt/x.cpp")
        r = self.run_ci(STUB_REDS=1)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertRegex(r.stdout, r"(?m)^ok   ui \(\d+ s, flaky: green on its rerun\)$")
        self.assertTrue(localci.passed(r.returncode, r.stdout))
        self.assertEqual(self.read("ui.runs").strip(), "2")
        fl = localci_flakes.read(self.out)
        self.assertEqual([(f["step"], f["tests"]) for f in fl],
                         [("ui", ["control", "session_saver"])])
        self.assertIn("session_saver", self.read("ui.first.log"))
        self.assertEqual(localci_steps.read_steps(self.out)["ui"]["state"], "pass")

    def test_red_twice_fails(self):
        self.change("ui/qt/x.cpp")
        r = self.run_ci(STUB_REDS=2)
        self.assertEqual(r.returncode, 1)
        self.assertRegex(r.stdout, r"(?m)^FAIL ui \(\d+ s, red twice\): ")
        self.assertFalse(localci.passed(r.returncode, r.stdout))
        self.assertEqual(localci_flakes.read(self.out), [])
        self.assertEqual(localci_steps.read_steps(self.out)["ui"]["state"], "fail")

    def test_a_build_error_is_not_rerun(self):
        self.change("ui/qt/x.cpp")
        r = self.run_ci(STUB_REDS=1, STUB_KIND="build")
        self.assertEqual(r.returncode, 1)
        self.assertEqual(self.read("ui.runs").strip(), "1")
        self.assertNotIn("rerun", r.stdout)

    def test_rerun_can_be_turned_off_by_hand(self):
        self.change("ui/qt/x.cpp")
        r = self.run_ci(STUB_REDS=1, SONORA_LOCAL_CI_RERUN=0)
        self.assertEqual(r.returncode, 1)
        self.assertEqual(self.read("ui.runs").strip(), "1")

    # -- the lock -----------------------------------------------------------
    def test_lock_wait_is_logged_apart_from_the_step_time(self):
        self.change("ui/qt/x.cpp")
        r = self.run_ci(STUB_WAIT=7)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertRegex(r.stdout, r"(?m)^ok   ui \(\d+ s, \+7 s waiting for the lock\)$")
        self.assertEqual(localci_steps.read_steps(self.out)["ui"]["wait"], 7)

    def test_an_orchestrator_run_holds_the_lock_for_all_its_steps(self):
        self.change("ui/qt/x.cpp")
        r = self.run_ci(STUB_WAIT=5, SONORA_LOCAL_CI_HOLD_LOCK=1)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        calls = self.read("slot.calls").splitlines()
        # one real acquisition, for the whole run; the step goes in held
        self.assertTrue(calls[0].startswith("held=0 bash ")
                        and calls[0].endswith("local-ci.sh --base main"), calls)
        self.assertTrue(all(c.startswith("held=1 ") for c in calls[1:]), calls)
        self.assertIn("local-ci: waited 5 s for the machine lock", r.stdout)
        self.assertRegex(r.stdout, r"(?m)^ok   ui \(\d+ s\)$")

    def test_a_harness_only_change_never_takes_the_lock(self):
        self.change("tools/harness/x.py")
        r = self.run_ci(SONORA_LOCAL_CI_HOLD_LOCK=1)
        self.assertIn("nothing to build", r.stdout)
        self.assertFalse(os.path.exists(os.path.join(self.out, "slot.calls")))


if __name__ == "__main__":
    unittest.main()
