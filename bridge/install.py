"""Wire ClaudeScreen into Claude Code.

    python install.py            add hooks + status line tap + start at login
    python install.py --remove   undo all of it

~/.claude/settings.json is backed up before it is changed. Your existing
status line command keeps running: the tap forwards to it.
"""
import argparse
import json
import os
import shutil
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
CLAUDE = os.path.join(os.path.expanduser("~"), ".claude")
SETTINGS = os.path.join(CLAUDE, "settings.json")
STATE_DIR = os.path.join(CLAUDE, "claude-screen")
CONFIG = os.path.join(STATE_DIR, "config.json")
STARTUP = os.path.join(os.environ.get("APPDATA", ""), r"Microsoft\Windows\Start Menu\Programs\Startup")
LAUNCHER = os.path.join(STARTUP, "ClaudeScreen.vbs")

EVENTS = ["SessionStart", "UserPromptSubmit", "PreToolUse", "PostToolUse", "PermissionRequest",
          "Notification", "Stop", "StopFailure", "SessionEnd"]
MARK = "claude-screen"


def py(script, windowless=False):
    exe = sys.executable
    if windowless:
        w = os.path.join(os.path.dirname(exe), "pythonw.exe")
        exe = w if os.path.exists(w) else exe
    return f'"{exe}" "{os.path.join(HERE, script)}"'.replace("\\", "/")


def is_ours(entry):
    return any(MARK in (h.get("command") or "") or "ClaudeScreen" in (h.get("command") or "")
               for h in entry.get("hooks", []))


def load():
    with open(SETTINGS, encoding="utf-8") as f:
        return json.load(f)


def save(settings):
    backup = f"{SETTINGS}.backup-claude-screen-{time.strftime('%Y%m%d-%H%M%S')}"
    shutil.copy2(SETTINGS, backup)
    with open(SETTINGS, "w", encoding="utf-8") as f:
        json.dump(settings, f, indent=2)
        f.write("\n")
    print(f"updated {SETTINGS} (backup: {os.path.basename(backup)})")


def install():
    os.makedirs(STATE_DIR, exist_ok=True)
    settings = load()
    hooks = settings.setdefault("hooks", {})
    for ev in EVENTS:
        entries = [e for e in hooks.get(ev, []) if not is_ours(e)]
        entry = {"hooks": [{"type": "command", "command": py("hook.py"), "async": True}]}
        if ev in ("PreToolUse", "PostToolUse", "PermissionRequest"):
            entry["matcher"] = "*"
        hooks[ev] = entries + [entry]

    current = settings.get("statusLine")
    tap = py("statusline.py")
    if not current or current.get("command") != tap:
        cfg = {}
        if os.path.exists(CONFIG):
            with open(CONFIG, encoding="utf-8") as f:
                cfg = json.load(f)
        cfg["previous_statusline"] = current
        with open(CONFIG, "w", encoding="utf-8") as f:
            json.dump(cfg, f, indent=2)
        settings["statusLine"] = {"type": "command", "command": tap, "padding": (current or {}).get("padding", 0)}
    save(settings)

    if os.path.isdir(STARTUP):
        cmd = py("claude_screen.py", windowless=True).replace('"', '""')
        with open(LAUNCHER, "w", encoding="utf-8") as f:
            f.write(f'CreateObject("WScript.Shell").Run "{cmd}", 0, False\n')
        print(f"bridge will start at login ({LAUNCHER})")


def remove():
    settings = load()
    hooks = settings.get("hooks", {})
    for ev in list(hooks):
        hooks[ev] = [e for e in hooks[ev] if not is_ours(e)]
        if not hooks[ev]:
            del hooks[ev]
    if os.path.exists(CONFIG):
        with open(CONFIG, encoding="utf-8") as f:
            prev = json.load(f).get("previous_statusline")
        if prev:
            settings["statusLine"] = prev
        else:
            settings.pop("statusLine", None)
    save(settings)
    if os.path.exists(LAUNCHER):
        os.remove(LAUNCHER)
        print("removed login launcher")


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--remove", action="store_true")
    remove() if ap.parse_args().remove else install()
