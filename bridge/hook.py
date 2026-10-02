"""Claude Code hook: append the event to the ClaudeScreen event log.

Registered for every hook event by install.py. Runs async, so it has to be
quick and must never fail loudly - Claude Code must not be disturbed by it.
"""
import json
import os
import sys
import time

STATE_DIR = os.path.join(os.path.expanduser("~"), ".claude", "claude-screen")
KEEP = ("session_id", "cwd", "hook_event_name", "tool_name", "notification_type",
        "message", "source", "model", "reason", "agent_id", "permission_mode")


def summarize_input(tool_input):
    if not isinstance(tool_input, dict):
        return ""
    for key in ("file_path", "notebook_path", "path", "url", "pattern", "query",
                "command", "description", "prompt", "skill"):
        v = tool_input.get(key)
        if isinstance(v, str) and v.strip():
            v = v.strip()
            if key in ("file_path", "notebook_path", "path"):
                v = os.path.basename(v.rstrip("/\\")) or v
            return v.splitlines()[0][:120]
    return ""


def main():
    ts = time.time()
    try:
        data = json.loads(sys.stdin.buffer.read().decode("utf-8", "replace") or "{}")
    except ValueError:
        return
    ev = {k: data[k] for k in KEEP if k in data}
    ev["ts"] = ts
    if "tool_input" in data:
        ev["input"] = summarize_input(data["tool_input"])
    last = data.get("last_assistant_message")
    if isinstance(last, str):
        ev["last"] = last[:300]
    os.makedirs(STATE_DIR, exist_ok=True)
    with open(os.path.join(STATE_DIR, "events.jsonl"), "a", encoding="utf-8") as f:
        f.write(json.dumps(ev, ensure_ascii=True) + "\n")


if __name__ == "__main__":
    try:
        main()
    except Exception:
        pass
