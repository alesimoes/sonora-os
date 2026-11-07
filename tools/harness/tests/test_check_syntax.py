"""tools/check_syntax.py: the YAML parse and the broken-literal check that
tools/pre-pr.sh and the Harness CI job run (2026-11-07: a printf '%s\n'
split in two lines made ci.yml unparsable and GitHub ran no jobs)."""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

import support  # noqa: F401  (sets sys.path)

TOOLS = os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__))))
sys.path.insert(0, TOOLS)
import check_syntax as cs  # noqa: E402

NL = chr(92) + "n"
BROKEN = "run: |\n  printf '%s" + "\n" + "' \"$files\"\n"
GOOD = "run: |\n  printf '%s" + NL + "' \"$files\"\n"


class Yaml(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def write(self, text):
        p = os.path.join(self.tmp, "w.yml")
        with open(p, "w", newline="\n") as f:
            f.write(text)
        return p

    def test_valid_file_passes(self):
        self.assertEqual(cs.yaml_errors(self.write("a:\n  - b: 1\n")), [])

    def test_invalid_file_reports_file_and_line(self):
        out = cs.yaml_errors(self.write("a: 1\nb: [1, 2\nc: 3\n"))
        self.assertEqual(len(out), 1)
        self.assertRegex(out[0], r"w\.yml:\d+: invalid YAML")

    def test_every_workflow_in_the_repo_parses(self):
        wf = os.path.join(os.path.dirname(TOOLS), ".github", "workflows")
        for n in sorted(os.listdir(wf)):
            if n.endswith(".yml"):
                self.assertEqual(cs.yaml_errors(os.path.join(wf, n)), [], n)


class Quotes(unittest.TestCase):
    def check(self, text, path="x.yml"):
        lines = text.split("\n")
        return cs.broken_literals(path, lines, set(range(1, len(lines) + 1)))

    def test_split_printf_is_flagged_with_its_line(self):
        out = self.check(BROKEN)
        self.assertEqual(len(out), 1)
        self.assertTrue(out[0].startswith("x.yml:2:"))

    def test_the_intact_literal_passes(self):
        self.assertEqual(self.check(GOOD), [])

    def test_double_quoted_cpp_split_is_flagged(self):
        self.assertEqual(len(self.check('f("a%s\n"b);\n', "x.cpp")), 1)

    def test_apostrophe_in_a_double_quoted_string_is_ignored(self):
        self.assertEqual(self.check('echo "it\'s fine"\n"x"\n'), [])

    def test_comment_is_ignored(self):
        self.assertEqual(self.check("# don't\n'x'\n"), [])

    def test_only_added_lines_are_checked(self):
        lines = BROKEN.split("\n")
        self.assertEqual(cs.broken_literals("x.yml", lines, {1}), [])

    def test_opt_out_marker(self):
        self.assertEqual(self.check("a: '1 # check_syntax: ok\n'b\n"), [])

    def test_cpp_raw_strings_are_skipped(self):
        self.assertEqual(self.check('auto s = R"(\nx "a\n"b)";\n', "x.cpp"), [])


class Cli(unittest.TestCase):
    def test_exit_codes(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp, True)
        bad = os.path.join(tmp, "bad.yml")
        with open(bad, "w") as f:
            f.write("a: [1\n")
        run = lambda *a: subprocess.run(
            [sys.executable, os.path.join(TOOLS, "check_syntax.py")] + list(a),
            capture_output=True, text=True)
        r = run("yaml", bad)
        self.assertEqual(r.returncode, 1)
        self.assertIn("bad.yml", r.stderr)
        self.assertEqual(run().returncode, 2)


if __name__ == "__main__":
    unittest.main()
