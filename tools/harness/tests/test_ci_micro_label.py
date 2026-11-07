"""tools/ci-micro-label.sh: the names of the micro build jobs."""
import shutil
import subprocess
import unittest

import support  # noqa: F401  (sets sys.path)



def label(mode, *files):
    r = subprocess.run(["bash", "ci-micro-label.sh", mode], cwd=support.TOOLS, input="\n".join(files),
                       capture_output=True, text=True, check=True)
    return r.stdout.strip()


@unittest.skipUnless(shutil.which("bash"), "needs bash")
class Label(unittest.TestCase):
    def test_build_names_the_contexts(self):
        self.assertEqual(label("build", "ui/qt/a.cpp", "common/model/x.cpp",
                               "docs/a.md", "engine/core/y.cpp"),
                         "Build + test: ui, model, engine-core (3 files)")

    def test_docs_only_builds_nothing(self):
        self.assertEqual(label("build", "docs/a.md", "tools/harness/x.py"),
                         "Nothing to build (docs/harness only)")

    def test_rtsan_counts_only_the_real_time_paths(self):
        self.assertEqual(label("rtsan", "ui/a.qml", "engine/core/y.cpp"),
                         "RTSan: engine-core (1 files)")
        self.assertEqual(label("rtsan", "ui/a.qml"),
                         "RTSan: no real-time code changed")

    def test_many_contexts_are_cut(self):
        out = label("build", "ui/a", "common/a/x", "common/b/x", "common/c/x",
                    "contracts/x")
        self.assertIn("+1", out)


if __name__ == "__main__":
    unittest.main()
