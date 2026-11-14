"""micro-build.sh must make the shared ccache volume writable by the container
user: the caller uid on Linux, uid 1000 elsewhere. A root-owned volume made
ccache fail every compile, which CMake reports as "Could NOT find Threads"."""
import os
import unittest

SCRIPT = os.path.join(os.path.dirname(__file__), "..", "..", "micro-build.sh")


class CcacheVolumeOwner(unittest.TestCase):
    def setUp(self):
        with open(SCRIPT, encoding="utf-8") as f:
            self.text = f.read()

    def test_named_volume_is_handed_to_caller(self):
        self.assertIn('own_volume sonora-ccache:/ccache "$owner"', self.text)
        self.assertIn('owner="$(id -u):$(id -g)"', self.text)

    def test_hand_over_is_not_limited_to_linux_callers(self):
        # Outside the AS_CALLER condition, uid 1000 default for the sonora
        # user off Linux, the whole tree so an existing volume is fixed.
        self.assertIn('owner="1000:1000"', self.text)
        i = self.text.index('own_volume sonora-ccache:/ccache')
        self.assertNotIn("then\n  own_volume", self.text[i - 120:i])

    def test_only_foreign_files_are_chowned(self):
        # chown -R rewrote 24k inodes per run (76 s on the USB disk,
        # 2026-11-13): the walk only reads, chown touches what is not ours.
        self.assertNotIn("chown -R", self.text)
        self.assertNotIn("-R 1000:1000", self.text)
        self.assertIn('\\( ! -user "$uid" -o ! -group "$gid" \\) -exec chown "$2" {} +',
                      self.text)


class PoisonedCacheRetry(unittest.TestCase):
    def test_entrypoint_retries_once_from_clean_cache(self):
        path = os.path.join(os.path.dirname(SCRIPT), "micro-build-entrypoint.sh")
        with open(path, encoding="utf-8") as f:
            text = f.read()
        self.assertIn("had_cache=1", text)
        self.assertIn('rm -rf "$BUILD/CMakeCache.txt" "$BUILD/CMakeFiles"', text)
        self.assertEqual(text.count("configure ||"), 1)  # one retry, no loop


if __name__ == "__main__":
    unittest.main()
