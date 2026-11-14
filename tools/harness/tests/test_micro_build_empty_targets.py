"""A plan with no targets must give an empty bash array: `print("")` still
emits a newline, so mapfile made one blank element and the entrypoint ran
`cmake --build --target ` (PR #669's gate)."""
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest

PATH = os.path.join(os.path.dirname(__file__), "..", "..", "micro-build-entrypoint.sh")


def extraction_line(text):
    return next(l for l in text.splitlines() if l.startswith("mapfile -t targets"))


class EmptyTargets(unittest.TestCase):
    def setUp(self):
        with open(PATH, encoding="utf-8") as f:
            self.text = f.read()

    def count(self, targets):
        bash = shutil.which("bash")
        if not bash:
            self.skipTest("no bash")
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
            json.dump({"targets": targets}, f)
        try:
            line = extraction_line(self.text).replace("python3", '"$PY"')
            script = line + '\necho "${#targets[@]}"'
            env = dict(os.environ, PY=sys.executable, PLAN=f.name.replace("\\", "/"))
            r = subprocess.run([bash, "-c", script], capture_output=True, text=True, env=env)
        finally:
            os.unlink(f.name)
        return r.stdout.strip()

    def test_empty_plan_gives_zero_targets(self):
        self.assertEqual(self.count([]), "0")

    def test_blank_entries_dropped_real_kept(self):
        self.assertEqual(self.count(["", "a", "b"]), "2")

    def test_build_is_guarded_by_a_non_empty_list(self):
        i = self.text.index('--target "${targets[@]}"')
        self.assertIn("if ((${#targets[@]}))", self.text[i - 150:i])


if __name__ == "__main__":
    unittest.main()
