"""What the supervisor touches outside the store: processes, the heavy lock
directory, the disk and the owner's desktop. One class, so the tests hand
the checks a fake with the same surface (tests/test_supervisor.py).

The lock directory is read directly (tools/build-slot.sh --list prints the
same files): a holder is slot-N/owner, a waiter is wait/<prio>-<ns>-<pid>,
both `key=value` per line as describe() in build-slot.sh writes them.
"""
import os
import re
import shutil
import subprocess
import sys
import time

from . import launcher, supervisor_dash

HOST_KIND = "win" if os.name == "nt" else "linux"


def lock_dir(env=None):
    """Where build-slot.sh keeps the lock on this host (its lock_dir())."""
    env = os.environ if env is None else env
    if env.get("SONORA_HEAVY_DIR"):
        return env["SONORA_HEAVY_DIR"]
    if os.name == "nt":
        base = env.get("LOCALAPPDATA") or os.path.join(
            env.get("USERPROFILE", ""), "AppData", "Local")
        return os.path.join(base, "sonora", "build-lock")
    return "/var/tmp/sonora-build-lock"


def _kv(path):
    out = {}
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                k, _, v = line.rstrip("\n").partition("=")
                if k:
                    out[k] = v
    except OSError:
        return None
    return out


def posix(path):
    """C:\\x, C:/x and /c/x compare equal; case-folded on Windows."""
    p = (path or "").replace("\\", "/")
    m = re.match(r"^/([a-zA-Z])/(.*)$", p)
    if m:
        p = f"{m.group(1)}:/{m.group(2)}"
    p = p.rstrip("/")
    return p.lower() if re.match(r"^[a-zA-Z]:/", p) else p


def under(path, root):
    p, r = posix(path), posix(root)
    return bool(p) and (p == r or p.startswith(r + "/"))


class RealSys:
    def __init__(self, lock=None, clock=time.time, notifier=None):
        self.lock = lock or lock_dir()
        self.clock = clock
        self._notify = notifier or desktop_notify

    # -- processes -----------------------------------------------------
    def pid_alive(self, pid, kind=None):
        """None when the pid belongs to another kind of host (a WSL pid seen
        from Windows) and cannot be probed."""
        if kind and kind != HOST_KIND:
            return None
        return launcher.pid_alive(pid)

    def kill(self, pid):
        launcher.kill_tree(pid)

    # -- the heavy lock --------------------------------------------------
    def lock_entries(self):
        """[{role: holds|waits, path, pid, kind, prio, start, cwd, cmd}]"""
        out = []
        try:
            names = sorted(os.listdir(self.lock))
        except OSError:
            return out
        for n in names:
            if not n.startswith("slot-"):
                continue
            d = os.path.join(self.lock, n)
            kv = _kv(os.path.join(d, "owner"))
            if kv is None:
                try:
                    age = self.clock() - os.stat(d).st_mtime
                except OSError:
                    continue
                kv = {"pid": "", "claiming": "1", "start": str(
                    int(self.clock() - age))}
            out.append(self._entry("holds", d, kv))
        wait = os.path.join(self.lock, "wait")
        try:
            tickets = sorted(os.listdir(wait))
        except OSError:
            tickets = []
        for t in tickets:
            p = os.path.join(wait, t)
            kv = _kv(p)
            if kv is not None:
                out.append(self._entry("waits", p, kv))
        return out

    @staticmethod
    def _entry(role, path, kv):
        def num(k):
            v = kv.get(k, "")
            return int(v) if v.isdigit() else None
        return {"role": role, "path": path, "pid": num("pid"),
                "kind": kv.get("kind", ""), "prio": num("prio"),
                "start": num("start"), "cwd": kv.get("cwd", ""),
                "cmd": kv.get("cmd", ""), "claiming": bool(kv.get("claiming"))}

    def lock_remove(self, entry):
        """A waiter's ticket is one file; a holder is its slot directory."""
        try:
            if entry["role"] == "waits":
                os.remove(entry["path"])
            else:
                shutil.rmtree(entry["path"], ignore_errors=True)
            return True
        except OSError:
            return False

    # -- disk ----------------------------------------------------------
    def disk_free_gb(self, path):
        try:
            return shutil.disk_usage(path).free / 1e9
        except OSError:
            return None

    # -- the owner -----------------------------------------------------
    def dash_up(self, port):
        return supervisor_dash.probe(port)

    def dash_start(self, port, repo_root):
        return supervisor_dash.start(repo_root, port)

    def notify(self, title, text):
        try:
            self._notify(title, text)
            return True
        except Exception:  # a lost toast must never stop a check
            return False


def desktop_notify(title, text, timeout=15):
    """Best effort, no third-party module: a Windows toast through
    PowerShell (msg.exe as the fallback), notify-send on Linux."""
    title, text = title[:80], text[:400]
    if sys.platform == "win32":
        ps = (
            "[Windows.UI.Notifications.ToastNotificationManager, "
            "Windows.UI.Notifications, ContentType = WindowsRuntime] > $null;"
            "$x=[Windows.UI.Notifications.ToastNotificationManager]::"
            "GetTemplateContent([Windows.UI.Notifications."
            "ToastTemplateType]::ToastText02);"
            "$t=$x.GetElementsByTagName('text');"
            "$t.Item(0).AppendChild($x.CreateTextNode($env:SONORA_N_TITLE))"
            ">$null;$t.Item(1).AppendChild($x.CreateTextNode("
            "$env:SONORA_N_TEXT))>$null;"
            "[Windows.UI.Notifications.ToastNotificationManager]::"
            "CreateToastNotifier('SonoraOS harness').Show("
            "[Windows.UI.Notifications.ToastNotification]::new($x))")
        env = dict(os.environ, SONORA_N_TITLE=title, SONORA_N_TEXT=text)
        r = subprocess.run(["powershell", "-NoProfile", "-NonInteractive",
                            "-Command", ps], env=env, capture_output=True,
                           timeout=timeout)
        if r.returncode != 0:
            subprocess.run(["msg.exe", "*", "/TIME:30",
                            f"{title}\n{text}"], capture_output=True,
                           timeout=timeout)
        return
    subprocess.run(["notify-send", "-u", "critical", title, text],
                   capture_output=True, timeout=timeout)
