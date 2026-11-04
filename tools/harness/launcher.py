"""Builds and starts the headless `claude -p` processes (ADR-0047 section 1).

The model flag lives in model_args() and nowhere else. build_command() has no
model parameter, so no caller, config file or environment variable can ask
for anything but Sonnet.
"""
import json
import os
import re
import secrets
import signal
import subprocess
import sys

MODEL = "sonnet"
FORBIDDEN_ENV = ("CLAUDE_MODEL", "ANTHROPIC_MODEL")
DEFAULT_TOOLS = ("Bash", "Read", "Edit", "Write", "MultiEdit", "Grep", "Glob",
                 "mcp__harness__whoami", "mcp__harness__send",
                 "mcp__harness__recv", "mcp__harness__claim",
                 "mcp__harness__release")
TOOLS_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class ModelError(Exception):
    pass


def model_args():
    return ["--model", MODEL]


def check_models(root, env=None):
    """Refuse to start when the environment or an agent file names another
    model. A file without a `model:` line inherits the --model flag."""
    env = os.environ if env is None else env
    for k in FORBIDDEN_ENV:
        if env.get(k) and env[k].strip().lower() != MODEL:
            raise ModelError(f"{k}={env[k]} but only {MODEL} is allowed")
    agents = os.path.join(root, ".claude", "agents")
    if not os.path.isdir(agents):
        return
    for fn in sorted(os.listdir(agents)):
        if not fn.endswith(".md"):
            continue
        with open(os.path.join(agents, fn), encoding="utf-8") as f:
            head = f.read(2000)
        m = re.match(r"---\r?\n(.*?)\r?\n---", head, re.S)
        for line in (m.group(1).splitlines() if m else ()):
            if line.startswith("model:") and \
                    line.split(":", 1)[1].strip().strip("'\"") != MODEL:
                raise ModelError(f"{fn} says {line.strip()}; "
                                 f"only {MODEL} is allowed")


def build_command(claude, agent, prompt, mcp_path, settings_path,
                  max_turns=80, permission_mode="acceptEdits",
                  tools=DEFAULT_TOOLS):
    argv = list(claude) if isinstance(claude, (list, tuple)) else [claude]
    argv += ["-p"] + model_args()
    argv += ["--agent", agent,
             "--mcp-config", str(mcp_path), "--strict-mcp-config",
             "--settings", str(settings_path),
             "--allowedTools", ",".join(tools),
             "--permission-mode", permission_mode,
             "--max-turns", str(max_turns),
             "--output-format", "stream-json", "--verbose",
             prompt]
    return argv


def claude_argv(fake_claude=None, which="claude"):
    if fake_claude:
        return [sys.executable, "-S", str(fake_claude)]  # -S: fast start
    return [which]


def hook_settings(harness_py):
    def cmd(*a):
        return " ".join([f'"{sys.executable}"', f'"{harness_py}"', *a])
    recv = {"type": "command", "command": cmd("recv", "--brief", "--hook")}
    return {"hooks": {
        "PostToolUse": [{"matcher": "*", "hooks": [recv]}],
        "UserPromptSubmit": [{"hooks": [recv]}],
        "PreToolUse": [{"matcher": "Edit|Write|MultiEdit", "hooks": [
            {"type": "command", "command": cmd("check", "--hook")}]}]}}


def mcp_config(env):
    return {"mcpServers": {"harness": {
        "command": sys.executable,
        "args": [os.path.join(TOOLS_DIR, "harness_mcp.py")],
        "env": env}}}


def run_env(name, task_id, feature, harness_dir, port, token, result_path):
    return {"SONORA_AGENT_NAME": name, "SONORA_HARNESS": str(harness_dir),
            "SONORA_HARNESS_PORT": str(port), "SONORA_HARNESS_TOKEN": token,
            "SONORA_TASK": task_id, "SONORA_FEATURE": feature or "",
            "SONORA_BROKER": f"127.0.0.1:{port}",
            "SONORA_TOKEN": token, "SONORA_RESULT_PATH": str(result_path)}


def new_token():
    return secrets.token_hex(32)


def write_run_files(run_dir, env):
    os.makedirs(run_dir, exist_ok=True)
    mcp = os.path.join(run_dir, "mcp.json")
    settings = os.path.join(run_dir, "settings.json")
    with open(mcp, "w", encoding="utf-8") as f:
        json.dump(mcp_config(env), f, indent=1)
    with open(settings, "w", encoding="utf-8") as f:
        json.dump(hook_settings(os.path.join(TOOLS_DIR, "harness.py")), f,
                  indent=1)
    return mcp, settings


def build_prompt(task, run_id, name, kind, result_path, extra=""):
    lines = [
        f"You are {name} ({kind}) in the SonoraOS harness.",
        f"task: {task['id']}  backlog: {task.get('backlog_id') or '-'}  "
        f"feature: {task.get('feature_id') or '-'}",
        f"branch: {task['branch']}  worktree: {task.get('worktree') or '-'}",
        f"reserved numbers: adr={task.get('adr_no')} "
        f"protocol={task.get('protocol_no')}",
        f"run: {run_id}",
        f"When finished write result.json to: {result_path}",
        "Schema: role, task, status (done|fail|blocked), summary, commit, "
        "branch, findings, followups, unverified.",
    ]
    if task.get("brief"):
        lines += ["", task["brief"]]
    if task.get("attempts"):
        lines += ["", "This is a relaunch: continue from `git log` and "
                  "`git diff origin/main...HEAD`; do not start over."]
    if task.get("note"):
        lines += ["", "note: " + task["note"]]
    if extra:
        lines += ["", extra]
    return "\n".join(lines)


class GitWorktrees:
    """One worktree per task under .claude/worktrees/<task>."""

    def __init__(self, root, base="origin/main"):
        self.root, self.base = root, base

    def path(self, task_id):
        return os.path.join(self.root, ".claude", "worktrees",
                            re.sub(r"[^\w.-]", "_", task_id))

    def ensure(self, task_id, branch):
        wt = self.path(task_id)
        if os.path.isdir(wt):
            return wt
        os.makedirs(os.path.dirname(wt), exist_ok=True)
        exists = subprocess.run(
            ["git", "-C", self.root, "rev-parse", "--verify", "-q", branch],
            capture_output=True).returncode == 0
        cmd = ["git", "-C", self.root, "worktree", "add"]
        cmd += [wt, branch] if exists else ["-b", branch, wt, self.base]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            raise RuntimeError("git worktree add: " + r.stderr.strip())
        return wt

    def remove(self, task_id):
        subprocess.run(["git", "-C", self.root, "worktree", "remove",
                        "--force", self.path(task_id)], capture_output=True)


def spawn(argv, env, cwd, stream_path):
    full = dict(os.environ)
    full.update(env)
    for k in FORBIDDEN_ENV:
        full.pop(k, None)
    out = open(stream_path, "ab")
    kw = {}
    if os.name == "nt":
        kw["creationflags"] = subprocess.CREATE_NEW_PROCESS_GROUP
    else:
        kw["start_new_session"] = True
    try:
        return subprocess.Popen(argv, cwd=cwd, env=full, stdout=out,
                                stderr=subprocess.STDOUT,
                                stdin=subprocess.DEVNULL, **kw)
    finally:
        out.close()


def kill_tree(pid):
    if not pid:
        return
    if os.name == "nt":
        subprocess.run(["taskkill", "/T", "/F", "/PID", str(pid)],
                       capture_output=True)
        return
    try:
        os.killpg(os.getpgid(pid), signal.SIGKILL)
    except (ProcessLookupError, PermissionError):
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass


def pid_alive(pid):
    if not pid:
        return False
    if os.name == "nt":
        import ctypes
        k32 = ctypes.windll.kernel32
        h = k32.OpenProcess(0x1000, False, int(pid))  # QUERY_LIMITED_INFO
        if not h:
            return False
        code = ctypes.c_ulong()
        ok = k32.GetExitCodeProcess(h, ctypes.byref(code))
        k32.CloseHandle(h)
        return bool(ok) and code.value == 259  # STILL_ACTIVE
    try:
        os.kill(pid, 0)
    except ProcessLookupError:
        return False
    except PermissionError:
        return True
    return True
