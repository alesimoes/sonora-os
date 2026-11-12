"""A plan whose only owner is a product executable no test runs has no
targets; the entrypoint must not call `cmake --build --target` with an empty
(or blank) name, which failed PR #669's gate."""
import os
import subprocess
import unittest

PATH = os.path.join(os.path.dirname(__file__), "..", "..", "micro-build-entrypoint.sh")
EXTRACT = 'import json,sys; print("\n".join(t for t in json.load(open(sys.argv[1]))["targets"] if t))'


class EmptyTargets(unittest.TestCase):
    def setUp(self):
        with open(PATH, encoding="utf-8") as f:
            self.text = f.read()

    def test_extraction_snippet_is_in_the_script(self):
        self.assertIn('for t in json.load(open(sys.argv[1]))["targets"] if t', self.text)

    def test_empty_plan_yields_no_blank_target(self):
        import json, sys, tempfile
        with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
            json.dump({"targets": []}, f)
        try:
            out = subprocess.run([sys.executable, "-c", EXTRACT, f.name],
                                 capture_output=True, text=True).stdout
        finally:
            os.unlink(f.name)
        self.assertEqual(out.split("\n")[0], "")
        self.assertEqual([x for x in out.split("\n") if x], [])

    def test_build_is_guarded_by_a_non_empty_list(self):
        i = self.text.index('--target "${targets[@]}"')
        self.assertIn("if ((${#targets[@]}))", self.text[i - 150:i])


if __name__ == "__main__":
    unittest.main()
