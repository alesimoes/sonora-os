"""The selected agent's activity (dashboard_activity.py): its last steps read
from the tail of the run's stream.log, and the GET route that serves them."""
import json
import os
import shutil
import tempfile
import unittest

import support  # noqa: F401  (sets sys.path)
import dashboard_activity as A


def assistant(ts, *content):
    return json.dumps({"type": "assistant", "timestamp": ts,
                       "message": {"content": list(content)}})


def tool(name, **inp):
    return {"type": "tool_use", "name": name, "input": inp}


class Activity(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.log = os.path.join(self.tmp, "stream.log")

    def write(self, *lines):
        with open(self.log, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")

    def test_tool_calls_and_text_become_steps_newest_last(self):
        self.write(json.dumps({"type": "system"}),
                   assistant("T1", {"type": "thinking", "thinking": "x"},
                             tool("Bash", command="git status", description="Show status")),
                   json.dumps({"type": "user", "message": {"content": []}}),
                   assistant("T2", {"type": "text", "text": "Done  with" + chr(10) + "the fix"}),
                   assistant("T3", tool("mcp__harness__send", to="lead", type="task_done",
                                        body={"summary": "ok"}),
                             tool("Read", file_path="a/b.py")),
                   "not json")
        s = A.steps(self.log)
        self.assertEqual([(x["kind"], x.get("tool"), x["text"]) for x in s], [
            ("tool", "Bash", "Show status"),
            ("text", None, "Done with the fix"),
            ("tool", "send", "→ lead (task_done) ok"),
            ("tool", "Read", "a/b.py")])
        self.assertEqual(s[0]["ts"], "T1")

    def test_only_the_tail_is_read_and_the_cut_line_dropped(self):
        self.write(*[assistant(str(i), tool("Grep", pattern="p%d" % i)) for i in range(400)])
        s = A.steps(self.log, limit=5)
        self.assertEqual([x["text"] for x in s], ["p395", "p396", "p397", "p398", "p399"])

    def test_route_answers_for_a_running_agent_only(self):
        self.write(assistant("T", tool("Edit", file_path="x.cpp")))
        orch = {"runs": [{"name": "eng-1", "task": "t1", "title": "Fix it", "model": "opus",
                          "stream_path": self.log}]}
        code, body = A.activity(orch, "eng-1")
        self.assertEqual((code, body["title"], body["steps"][0]["text"]), (200, "Fix it", "x.cpp"))
        self.assertEqual(A.activity(orch, "eng-2")[0], 404)
        self.assertEqual(A.activity({"runs": [{"name": "eng-3"}]}, "eng-3")[1]["steps"], [])

    def test_worktree_prefixes_are_cut_and_other_inputs_shown(self):
        wt = r"C:\source\sound\.claude\worktrees\agent-a1\.claude\worktrees\ui.x" + "\\"
        self.write(assistant("T", tool("Read", file_path=wt + r"ui\a.cpp"),
                             tool("Bash", command="cd /c/s/.claude/worktrees/ui.x && make"),
                             tool("mcp__harness__claim", files=["ui/a.cpp"])))
        self.assertEqual([x["text"] for x in A.steps(self.log)],
                         [r"ui\a.cpp", "cd && make", '["ui/a.cpp"]'])

    def test_a_missing_log_gives_no_steps(self):
        self.assertEqual(A.steps(os.path.join(self.tmp, "nope.log")), [])


if __name__ == "__main__":
    unittest.main()
