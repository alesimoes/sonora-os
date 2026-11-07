import unittest

import support  # noqa: F401  (puts tools/ on sys.path)
from harness import findings


class BlockingFindings(unittest.TestCase):
    def test_objects_by_severity(self):
        got = findings.blocking([{"severity": "blocking", "text": "a"},
                                 {"severity": "minor", "text": "b"}])
        self.assertEqual([f["text"] for f in got], ["a"])

    def test_plain_strings_do_not_raise(self):
        # 2026-11-07: a string finding made every daemon tick raise.
        got = findings.blocking(["note: rename x", "Blocking: leaks fd"])
        self.assertEqual(got, [{"severity": "blocking",
                                "text": "Blocking: leaks fd"}])

    def test_missing_or_odd_values(self):
        self.assertEqual(findings.blocking(None), [])
        self.assertEqual(findings.blocking([1, None, ["x"]]), [])


if __name__ == "__main__":
    unittest.main()
