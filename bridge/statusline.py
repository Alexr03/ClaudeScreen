"""Claude Code statusLine tap.

Claude Code hands the status line its plan rate limits, model and context
usage. This saves that JSON for the ClaudeScreen bridge, then either runs the
status line command you had before (saved by install.py) or prints a compact
line of its own.
"""
import json
import os
import shutil
import subprocess
import sys
import time

STATE_DIR = os.path.join(os.path.expanduser("~"), ".claude", "claude-screen")


def save(data):
    os.makedirs(STATE_DIR, exist_ok=True)
    sid = data.get("session_id") or "unknown"
    snap = {
        "ts": time.time(),
        "session_id": sid,
        "model": (data.get("model") or {}).get("display_name"),
        "context": (data.get("context_window") or {}).get("used_percentage"),
        "cwd": (data.get("workspace") or {}).get("current_dir") or data.get("cwd"),
        "project": (data.get("workspace") or {}).get("project_dir"),
    }
    path = os.path.join(STATE_DIR, f"status-{sid}.json")
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(snap, f)
    os.replace(tmp, path)

    limits = data.get("rate_limits")
    if isinstance(limits, dict) and limits:
        path = os.path.join(STATE_DIR, "rate_limits.json")
        with open(path + ".tmp", "w", encoding="utf-8") as f:
            json.dump({"ts": time.time(), "rate_limits": limits}, f)
        os.replace(path + ".tmp", path)


def chained_command():
    try:
        with open(os.path.join(STATE_DIR, "config.json"), encoding="utf-8") as f:
            return json.load(f).get("previous_statusline")
    except (OSError, ValueError):
        return None


def fallback(data):
    parts = [(data.get("model") or {}).get("display_name") or "Claude"]
    ctx = (data.get("context_window") or {}).get("used_percentage")
    if ctx is not None:
        parts.append(f"ctx {ctx:.0f}%")
    five = ((data.get("rate_limits") or {}).get("five_hour") or {}).get("used_percentage")
    if five is not None:
        parts.append(f"5h {five:.0f}%")
    return " | ".join(parts)


def main():
    raw = sys.stdin.buffer.read()
    try:
        data = json.loads(raw.decode("utf-8", "replace") or "{}")
    except ValueError:
        data = {}
    try:
        save(data)
    except Exception:
        pass

    prev = chained_command()
    if prev and prev.get("command"):
        try:
            # Claude Code runs status line commands through bash when it has
            # one (Git Bash on Windows), so do the same for the chained one.
            bash = shutil.which("bash")
            args = [bash, "-c", prev["command"]] if bash else prev["command"]
            out = subprocess.run(args, input=raw, capture_output=True, shell=not bash, timeout=10)
            if out.returncode == 0 and out.stdout.strip():
                sys.stdout.buffer.write(out.stdout)
                return
        except Exception:
            pass
    print(fallback(data))


if __name__ == "__main__":
    main()
