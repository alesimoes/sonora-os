import unittest

import support  # noqa: F401  (puts tools/ on sys.path)
from harness import proposal


class Normalize(unittest.TestCase):
    def test_canonical_body_is_kept(self):
        b = {"feature": "ui.x", "track": "daw", "backlog_ids": ["ui.x"],
             "brief": "why"}
        self.assertEqual(proposal.normalize(b), b)

    def test_shapes_leads_sent_on_2026_11_08(self):
        got = proposal.normalize({"items": ["drivers.usb.virtual_card"],
                                  "reason": "no hardware", "track": "os"})
        self.assertEqual(got, {"feature": "drivers.usb.virtual_card",
                               "track": "os",
                               "backlog_ids": ["drivers.usb.virtual_card"],
                               "brief": "no hardware"})
        got = proposal.normalize({"ids": ["a.b"], "note": "n", "track": "os"})
        self.assertEqual((got["feature"], got["brief"]), ("a.b", "n"))
        got = proposal.normalize({"track": "daw", "proposals": [
            {"ids": ["ui.editing.clip_gain"], "why": "vocal"},
            {"ids": ["ui.editing.import"], "why": "import"}]})
        self.assertEqual((got["feature"], got["track"], got["brief"]),
                         ("ui.editing.clip_gain", "daw", "vocal"))

    def test_no_ids_is_rejected(self):
        self.assertIsNone(proposal.normalize({"track": "daw"}))
        self.assertIsNone(proposal.normalize("text"))
        self.assertIsNone(proposal.normalize({"items": []}))


if __name__ == "__main__":
    unittest.main()
