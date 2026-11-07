"""tools/micro_targets.py: changed paths -> targets -> tests, against a
hand-written codemodel (ADR-0061). No cmake runs here."""
import json
import os
import sys
import tempfile
import unittest

import support  # noqa: F401  (sets sys.path)
import micro_targets as mt


def T(name, dir_, sources=(), deps=(), type_="STATIC_LIBRARY", artifacts=()):
    return {"name": name, "id": name + "::@1", "type": type_, "dir": dir_,
            "sources": list(sources), "deps": [d + "::@1" for d in deps],
            "artifacts": list(artifacts) or [name]}


# common/base <- common/model <- engine/core <- engine/app (sonora-engine)
# common/base <- common/dsp <- native_eq ; tests per module.
TARGETS = [
    T("sonora_base", "common/base", ["common/base/src/ring.cpp"]),
    T("test_base", "common/base", ["common/base/tests/test_ring.cpp"],
      ["sonora_base"], "EXECUTABLE"),
    T("sonora_model", "common/model", ["common/model/src/apply.cpp"],
      ["sonora_base"]),
    T("test_model", "common/model", ["common/model/tests/test_apply.cpp"],
      ["sonora_model"], "EXECUTABLE"),
    T("sonora_core", "engine/core", ["engine/core/src/session.cpp"],
      ["sonora_model", "sonora_base"]),
    T("test_core", "engine/core/tests", ["engine/core/tests/test_session.cpp"],
      ["sonora_core"], "EXECUTABLE"),
    T("sonora-engine", "engine/app", ["engine/app/src/main.cpp"],
      ["sonora_core"], "EXECUTABLE"),
    T("sonora_dsp", "common/dsp", ["common/dsp/src/biquad.cpp"], ["sonora_base"]),
    T("native_eq", "plugins/native/eq", ["plugins/native/eq/eq.cpp"],
      ["sonora_dsp"], "SHARED_LIBRARY"),
    T("test_native_eq", "plugins/native/eq", ["plugins/native/eq/test_eq.cpp"],
      ["native_eq"], "EXECUTABLE"),
    T("portable", "", [], ["test_base", "test_model"], "UTILITY"),
]
CTEST = {"tests": [
    {"name": "base_ring", "command": ["/b/test_base"]},
    {"name": "model_apply", "command": ["/b/test_model"]},
    {"name": "core_session", "command": ["/b/test_core"]},
    {"name": "native_eq", "command": ["/b/test_native_eq", "--quick"]},
    {"name": "engine_failed_take", "command": [
        "python3", "tools/failed-take-quit.py", "--engine", "/b/sonora-engine"]},
]}


class Plan(unittest.TestCase):
    def names(self, files, **kw):
        p = mt.plan(files, TARGETS, **kw)
        return p["scope"], p["targets"]

    def test_a_source_file_builds_its_target_and_every_dependent(self):
        scope, names = self.names(["common/model/src/apply.cpp"])
        self.assertEqual(scope, "targets")
        self.assertEqual(names, ["sonora-engine", "sonora_core", "sonora_model",
                                 "test_core", "test_model"])

    def test_direct_stops_at_the_first_dependents(self):
        _, names = self.names(["common/model/src/apply.cpp"], direct=True)
        self.assertEqual(names, ["sonora_core", "sonora_model", "test_model"])

    def test_a_header_is_owned_by_its_directory(self):
        scope, names = self.names(["common/dsp/include/sonora/dsp/biquad.h"])
        self.assertEqual(scope, "targets")
        self.assertEqual(names, ["native_eq", "sonora_dsp", "test_native_eq"])

    def test_the_deepest_directory_wins(self):
        # engine/core/tests/CMakeLists.txt belongs to the test target, not to
        # sonora_core and so not to the engine.
        _, names = self.names(["engine/core/tests/CMakeLists.txt"])
        self.assertEqual(names, ["test_core"])

    def test_a_leaf_change_touches_nothing_else(self):
        _, names = self.names(["plugins/native/eq/eq.cpp"])
        self.assertEqual(names, ["native_eq", "test_native_eq"])

    def test_docs_and_harness_build_nothing(self):
        scope, names = self.names(["docs/x.md", "tools/harness/train.py",
                                   "planning/BACKLOG.md"])
        self.assertEqual((scope, names), ("none", []))

    def test_root_build_files_mean_the_whole_tree(self):
        for f in ("CMakeLists.txt", "cmake/sonora.cmake", "tools/x.cmake"):
            scope, names = self.names([f])
            self.assertEqual(scope, "all", f)
            self.assertIn("sonora-engine", names)
            self.assertNotIn("", names)

    def test_a_root_utility_target_owns_no_stray_file(self):
        scope, _ = self.names(["README.txt"])
        self.assertEqual(scope, "none")

    def test_why_names_the_owner_per_path(self):
        p = mt.plan(["common/base/src/ring.cpp", "docs/a.md"], TARGETS)
        self.assertEqual(p["why"]["common/base/src/ring.cpp"],
                         "owned by sonora_base")
        self.assertIn("nothing", p["why"]["docs/a.md"])

    def test_windows_and_dot_slash_paths_are_normalised(self):
        _, names = self.names(["./common\\dsp\\src\\biquad.cpp"])
        self.assertIn("sonora_dsp", names)


class Tests(unittest.TestCase):
    def test_tests_are_the_ones_running_an_affected_artifact(self):
        p = mt.plan(["common/model/src/apply.cpp"], TARGETS)
        tests = mt.match_tests(CTEST, TARGETS, p["targets"])
        self.assertEqual(tests, ["core_session", "engine_failed_take",
                                 "model_apply"])

    def test_a_script_test_is_matched_by_the_binary_it_names(self):
        tests = mt.match_tests(CTEST, TARGETS, ["sonora-engine"])
        self.assertEqual(tests, ["engine_failed_take"])

    def test_an_object_library_a_test_names_is_built_with_it(self):
        canary = T("canary_alloc", "engine/pc", ["engine/pc/tests/canary_alloc.cpp"],
                   type_="OBJECT_LIBRARY")
        other = T("canary_other", "engine/pc", ["engine/pc/tests/other.cpp"],
                  type_="OBJECT_LIBRARY")
        listing = {"tests": [{"name": "rt_canary_alloc", "command": [
            "cmake", "-DRT_OBJECTS=/b/x/canary_alloc.cpp.o;/b/y/z.cpp.o",
            "-P", "rt.cmake"]}]}
        self.assertEqual(mt.object_targets_for(listing, [canary, other],
                                               ["rt_canary_alloc"]),
                         {"canary_alloc"})
        self.assertEqual(mt.object_targets_for(listing, [canary, other], []),
                         set())

    def test_an_app_executable_is_linked_only_when_a_test_runs_it(self):
        p = mt.plan(["common/model/src/apply.cpp"], TARGETS)
        self.assertIn("sonora-engine", p["targets"])
        no_engine_test = {"tests": [t for t in CTEST["tests"]
                                    if t["name"] != "engine_failed_take"]}
        kept = mt.prune_apps(TARGETS, p["targets"], no_engine_test)
        self.assertNotIn("sonora-engine", kept)
        self.assertEqual(kept, ["sonora_core", "sonora_model", "test_core",
                                "test_model"])
        # with the engine-process test registered, the engine is needed
        self.assertIn("sonora-engine", mt.prune_apps(TARGETS, p["targets"], CTEST))

    def test_regex_matches_exact_names_only(self):
        import re
        rx = re.compile(mt.ctest_regex(["native_eq", "core_session"]))
        self.assertTrue(rx.match("native_eq"))
        self.assertFalse(rx.match("native_eq_slow"))


class CTestFiles(unittest.TestCase):
    def test_reads_names_and_commands_from_the_generated_files(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(__import__("shutil").rmtree, tmp, True)
        os.makedirs(os.path.join(tmp, "common", "json"))
        with open(os.path.join(tmp, "CTestTestfile.cmake"), "w") as f:
            f.write("# CMake generated Testfile" + "\n"
                    + "subdirs(\"common/json\")" + "\n")
        with open(os.path.join(tmp, "common", "json", "CTestTestfile.cmake"), "w") as f:
            f.write("\n".join([
                'add_test([=[json]=] "/b/test_json")',
                'set_tests_properties([=[json]=] PROPERTIES LABELS "json")',
                'add_test(engine_fake_record "/usr/bin/cmake" '
                '"-DENGINE=/b/sonora-engine" "-P" "/src/x.cmake")',
                'add_test([=[two words]=] "/b/test_a" "--quick"',
                '  "--seed" "1")']) + "\n")
        got = mt.ctest_listing(tmp)["tests"]
        self.assertEqual([t["name"] for t in got],
                         ["json", "engine_fake_record", "two words"])
        self.assertEqual(got[0]["command"], ["/b/test_json"])
        self.assertEqual(got[2]["command"], ["/b/test_a", "--quick", "--seed", "1"])
        # the -DENGINE= word names the engine: the plan runs it when built
        self.assertEqual(mt.match_tests({"tests": got}, TARGETS, ["sonora-engine"]),
                         ["engine_fake_record"])


class FileApi(unittest.TestCase):
    """read_codemodel against a reply laid out as cmake writes it."""
    def test_reads_targets_sources_deps_and_dirs(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(__import__("shutil").rmtree, tmp, True)
        mt.write_query(tmp)
        self.assertTrue(os.path.exists(os.path.join(
            tmp, ".cmake", "api", "v1", "query", mt.CLIENT, "query.json")))
        reply = os.path.join(tmp, ".cmake", "api", "v1", "reply")
        os.makedirs(reply)
        src = "/src" if sys.platform != "win32" else "C:/src"

        def dump(name, obj):
            with open(os.path.join(reply, name), "w") as f:
                json.dump(obj, f)
        dump("target-a.json", {
            "name": "sonora_base", "id": "a::@1", "type": "STATIC_LIBRARY",
            "paths": {"source": "common/base", "build": "common/base"},
            "sources": [{"path": "common/base/src/ring.cpp"}],
            "artifacts": [{"path": "common/base/libsonora_base.a"}]})
        dump("target-b.json", {
            "name": "test_base", "id": "b::@1", "type": "EXECUTABLE",
            "paths": {"source": "common/base/tests", "build": "x"},
            "sources": [{"path": src + "/common/base/tests/t.cpp"}],
            "dependencies": [{"id": "a::@1"}],
            "artifacts": [{"path": "test_base"}]})
        dump("codemodel-v2.json", {
            "paths": {"source": src, "build": tmp},
            "configurations": [{"targets": [
                {"jsonFile": "target-a.json"}, {"jsonFile": "target-b.json"}]}]})
        dump("index-2026.json", {"reply": {mt.CLIENT: {"query.json": {
            "responses": [{"kind": "codemodel", "jsonFile": "codemodel-v2.json"}]}}}})
        targets, root = mt.read_codemodel(tmp)
        self.assertEqual(root, src)
        by = {t["name"]: t for t in targets}
        self.assertEqual(by["sonora_base"]["dir"], "common/base")
        self.assertEqual(by["test_base"]["sources"],
                         ["common/base/tests/t.cpp"])
        self.assertEqual(by["test_base"]["deps"], ["a::@1"])
        p = mt.plan(["common/base/include/ring.h"], targets)
        self.assertEqual(p["targets"], ["sonora_base", "test_base"])


if __name__ == "__main__":
    unittest.main()
