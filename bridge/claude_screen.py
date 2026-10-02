"""ClaudeScreen bridge: feeds Claude Code activity + plan usage to the display.

    python claude_screen.py                 run (auto-detects the CH340 port)
    python claude_screen.py --port COM4     run on a specific port
    python claude_screen.py --demo          cycle through demo states
    python claude_screen.py --shot out.png  save a screenshot of the display

Inputs, all under ~/.claude/claude-screen/ (written by hook.py / statusline.py):
    events.jsonl        one line per Claude Code hook event
    status-<sid>.json   model + context % per session, from the status line
    rate_limits.json    5-hour / weekly plan usage, from the status line
"""
import argparse
import glob
import json
import os
import sys
import time
import unicodedata
from datetime import datetime

import serial
import serial.tools.list_ports

STATE_DIR = os.path.join(os.path.expanduser("~"), ".claude", "claude-screen")
EVENTS = os.path.join(STATE_DIR, "events.jsonl")
BAUD = 460800
REPLAY_BYTES = 1 << 20
ROTATE_BYTES = 8 << 20

IDLE_FORGET = 3 * 3600   # drop finished sessions after this long
STALL_AFTER = 15 * 60    # "working" with no events this long -> assume interrupted


# ------------------------------------------------------------ text helpers --

ALLOWED = set("·—•…")


def clean(s, limit=90):
    """Reduce to what the display's fonts can draw (ASCII + a few symbols)."""
    s = " ".join(str(s or "").split())
    s = s.replace("‘", "'").replace("’", "'").replace("“", '"').replace("”", '"')
    out = []
    for ch in s:
        if ch.isascii() or ch in ALLOWED:
            out.append(ch)
        else:
            out.append(unicodedata.normalize("NFKD", ch).encode("ascii", "ignore").decode())
    s = "".join(out).strip()
    return s if len(s) <= limit else s[: limit - 1] + "…"


def first_line(s):
    for line in str(s or "").splitlines():
        line = line.strip().lstrip("#*->` ").strip()
        if line:
            return line
    return ""


def pretty_model(m):
    if not m:
        return ""
    if not m.startswith("claude-"):
        return clean(m, 20)
    parts = m.split("-")[1:]
    name = parts[0].capitalize() if parts else m
    ver = ".".join(p for p in parts[1:3] if p.isdigit() and len(p) <= 2)
    return f"{name} {ver}".strip()


TOOL_VERBS = {
    "Edit": "Editing", "MultiEdit": "Editing", "Write": "Writing", "NotebookEdit": "Editing",
    "Read": "Reading", "Glob": "Searching", "Grep": "Searching", "LS": "Searching",
    "Bash": "Running", "PowerShell": "Running", "BashOutput": "Running", "Monitor": "Watching",
    "WebFetch": "Researching", "WebSearch": "Researching",
    "Task": "Delegating", "Agent": "Delegating", "Workflow": "Orchestrating",
    "TodoWrite": "Planning", "TaskCreate": "Planning", "TaskUpdate": "Planning",
    "Skill": "Using a skill", "LSP": "Inspecting",
}
WAITING_TOOLS = {"AskUserQuestion": ("Question", "Claude is asking you something"),
                 "ExitPlanMode": ("Plan ready", "Review the plan to continue")}


def tool_verb(tool):
    if tool in TOOL_VERBS:
        return TOOL_VERBS[tool]
    if tool.startswith("mcp__"):
        return "Browsing" if "rowser" in tool or "chrome" in tool else "Using tools"
    return "Working"


def tool_label(tool):
    if tool.startswith("mcp__"):
        return tool.split("__")[-1]
    return tool


# ---------------------------------------------------------------- tracker --

class Session:
    def __init__(self, sid, cwd, ts):
        self.sid = sid
        self.cwd = cwd or ""
        self.state = "idle"
        self.verb = "Ready"
        self.detail = "session started"
        self.since = ts          # start of the current state
        self.turn_start = ts
        self.last_ts = ts
        self.last_event = ""
        self.model = ""
        self.transcript = ""

    @property
    def name(self):
        return os.path.basename(self.cwd.rstrip("/\\")) or "session"


class Tracker:
    def __init__(self):
        self.sessions = {}
        self.offset = 0

    # Hook events can arrive slightly out of order (hooks run async), so each
    # session ignores anything older than what it has already applied.
    def apply(self, ev):
        sid, ts, name = ev.get("session_id"), ev.get("ts", 0), ev.get("hook_event_name", "")
        if not sid:
            return
        if name == "SessionEnd":
            self.sessions.pop(sid, None)
            return
        s = self.sessions.get(sid)
        if s is None:
            s = self.sessions[sid] = Session(sid, ev.get("cwd"), ts)
        if ts < s.last_ts:
            return
        s.last_ts = ts
        s.last_event = name
        if ev.get("transcript_path"):
            s.transcript = ev["transcript_path"]
        if name == "SessionStart" and ev.get("cwd"):
            s.cwd = ev["cwd"]
        tool = ev.get("tool_name") or ""
        inp = ev.get("input") or ""

        def set_state(state, verb, detail, since=None):
            if state != s.state:
                s.since = since if since is not None else ts
            s.state, s.verb, s.detail = state, verb, detail

        if name == "SessionStart":
            s.model = pretty_model(ev.get("model")) or s.model
            set_state("idle", "Ready", "session " + (ev.get("source") or "started"), ts)
        elif name == "UserPromptSubmit":
            s.turn_start = ts
            set_state("work", "Thinking", "", ts)
            s.since = ts
        elif name == "PreToolUse":
            if tool in WAITING_TOOLS:
                verb, detail = WAITING_TOOLS[tool]
                set_state("wait", verb, detail)
            else:
                set_state("work", tool_verb(tool), clean(f"{tool_label(tool)}  {inp}".strip()), s.turn_start)
        elif name == "PostToolUse":
            set_state("work", "Thinking", "", s.turn_start)
        elif name == "PermissionRequest":
            set_state("wait", "Needs you", clean(f"Allow {tool_label(tool)}?  {inp}".strip()))
        elif name == "Notification":
            kind = ev.get("notification_type") or ""
            if kind in ("permission_prompt", "elicitation_dialog"):
                if s.state != "wait":
                    set_state("wait", "Needs you", clean(ev.get("message") or "Waiting for you"))
        elif name == "Stop" and not ev.get("agent_id"):
            took = ts - s.turn_start
            msg = clean(first_line(ev.get("last")))
            set_state("idle", "Done", msg or f"finished in {fmt_dur(took)}", ts)
        elif name == "StopFailure":
            set_state("idle", "Stopped", clean(ev.get("message") or "the turn ended with an error"), ts)

    def read_new(self, replay=False):
        try:
            size = os.path.getsize(EVENTS)
        except OSError:
            return False
        if size < self.offset:
            self.offset = 0  # log was rotated
        if replay:
            self.offset = max(0, size - REPLAY_BYTES)
        if size == self.offset:
            return False
        with open(EVENTS, "rb") as f:
            f.seek(self.offset)
            chunk = f.read()
        end = chunk.rfind(b"\n") + 1  # leave a partially written line for later
        self.offset += end
        events = []
        for line in chunk[:end].splitlines():
            try:
                events.append(json.loads(line))
            except ValueError:
                continue
        for ev in sorted(events, key=lambda e: e.get("ts", 0)):
            self.apply(ev)
        if self.offset > ROTATE_BYTES:
            try:
                os.replace(EVENTS, EVENTS + ".old")
                self.offset = 0
            except OSError:
                pass
        return bool(events)

    def prune(self, now):
        for sid, s in list(self.sessions.items()):
            quiet = now - s.last_ts
            if s.state == "idle" and quiet > IDLE_FORGET:
                del self.sessions[sid]
            elif s.state == "work" and quiet > STALL_AFTER and s.last_event != "PreToolUse":
                s.state, s.verb, s.detail, s.since = "idle", "Interrupted", "no activity for a while", s.last_ts


# ---------------------------------------------------------------- payload --

def fmt_dur(sec):
    sec = int(sec)
    if sec < 60:
        return f"{sec}s"
    if sec < 3600:
        return f"{sec // 60}m {sec % 60:02d}s"
    return f"{sec // 3600}h {sec % 3600 // 60:02d}m"


def read_json(path):
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def to_epoch(v):
    if isinstance(v, (int, float)):
        return v / 1000 if v > 1e12 else v
    if isinstance(v, str):
        try:
            return datetime.fromisoformat(v.replace("Z", "+00:00")).timestamp()
        except ValueError:
            return None
    return None


def limits(now):
    data = read_json(os.path.join(STATE_DIR, "rate_limits.json")) or {}
    rl = data.get("rate_limits") or {}
    out = {}
    for key, name in (("h5", "five_hour"), ("d7", "seven_day")):
        w = rl.get(name) or {}
        pct, reset = w.get("used_percentage", w.get("utilization")), to_epoch(w.get("resets_at"))
        if pct is not None and reset is not None:
            out[key] = [round(float(pct), 1), int(reset - now)]
    return out


USAGE_URL = "https://api.anthropic.com/api/oauth/usage"
USAGE_EVERY = 180


def fetch_usage():
    """Plan usage as /usage shows it, using Claude Code's own login.

    The token is read fresh each time (Claude Code refreshes it) and only ever
    sent to api.anthropic.com. Writes the same file the status line tap does,
    so whichever source is newer wins.
    """
    import urllib.request

    with open(os.path.join(os.path.expanduser("~"), ".claude", ".credentials.json"), encoding="utf-8") as f:
        token = json.load(f)["claudeAiOauth"]["accessToken"]
    req = urllib.request.Request(USAGE_URL, headers={
        "Authorization": f"Bearer {token}",
        "anthropic-beta": "oauth-2025-04-20",
        "User-Agent": "claude-screen",
    })
    with urllib.request.urlopen(req, timeout=10) as r:
        data = json.load(r)
    limits = {k: data[k] for k in ("five_hour", "seven_day") if isinstance(data.get(k), dict)}
    if not limits:
        raise ValueError(f"unexpected usage response keys: {sorted(data)}")
    path = os.path.join(STATE_DIR, "rate_limits.json")
    with open(path + ".tmp", "w", encoding="utf-8") as f:
        json.dump({"ts": time.time(), "source": "api", "rate_limits": limits}, f)
    os.replace(path + ".tmp", path)


_model_cache = {}


def transcript_model(path):
    """The model of the latest reply in a session transcript.

    Covers sessions started before the hooks were installed, and /model
    switches mid-session. Only the tail of the file is read, and the result is
    cached until the file changes.
    """
    try:
        mtime = os.path.getmtime(path)
    except (OSError, TypeError):
        return ""
    hit = _model_cache.get(path)
    if hit and hit[0] == mtime:
        return hit[1]
    model = ""
    with open(path, "rb") as f:
        f.seek(0, os.SEEK_END)
        f.seek(max(0, f.tell() - 262144))
        tail = f.read().decode("utf-8", "replace")
    for line in reversed(tail.splitlines()):
        if '"model"' not in line:
            continue
        try:
            m = (json.loads(line).get("message") or {}).get("model")
        except ValueError:
            continue
        if m and m.startswith("claude-"):
            model = pretty_model(m)
            break
    _model_cache[path] = (mtime, model)
    return model


def build_payload(tracker, now):
    statuses = {}
    for path in glob.glob(os.path.join(STATE_DIR, "status-*.json")):
        st = read_json(path)
        if st and st.get("session_id"):
            statuses[st["session_id"]] = st
        elif now - os.path.getmtime(path) > 86400:
            os.remove(path)

    order = {"wait": 0, "work": 1, "idle": 2}
    sessions = sorted(tracker.sessions.values(), key=lambda s: (order[s.state], -s.last_ts))
    out = []
    for s in sessions[:6]:
        st = statuses.get(s.sid) or {}
        ctx = st.get("context")
        name = os.path.basename((st.get("project") or "").rstrip("/\\")) or s.name
        out.append({
            "n": clean(name, 38),
            "st": s.state,
            "v": clean(s.verb, 22),
            "d": clean(s.detail, 90),
            "a": max(0, int(now - s.since)),
            "c": int(round(ctx)) if isinstance(ctx, (int, float)) else -1,
            "m": clean(st.get("model") or transcript_model(s.transcript) or s.model, 22),
        })
    return {"t": datetime.now().strftime("%H:%M"), "s": out, "l": limits(now)}


DEMO = [
    {"s": [{"n": "ClaudeScreen", "st": "work", "v": "Editing", "d": "Edit  main.cpp", "a": 134, "c": 42, "m": "Opus 5.5"},
           {"n": "strp2020", "st": "idle", "v": "Done", "d": "All tests pass", "a": 610, "c": 18, "m": "Sonnet 5.5"}],
     "l": {"h5": [23.5, 8040], "d7": [41.0, 302400]}},
    {"s": [{"n": "strp2020", "st": "wait", "v": "Needs you", "d": "Allow Bash?  kubectl rollout status", "a": 12, "c": 63, "m": "Opus 5.5"},
           {"n": "ClaudeScreen", "st": "work", "v": "Running", "d": "Bash  pio run -e cyd", "a": 201, "c": 44, "m": "Opus 5.5"}],
     "l": {"h5": [78.0, 3100], "d7": [56.2, 290000]}},
    {"s": [{"n": "ClaudeScreen", "st": "idle", "v": "Done", "d": "Flashed the firmware and verified the display", "a": 75, "c": 47, "m": "Opus 5.5"}],
     "l": {"h5": [93.0, 1500], "d7": [64.0, 280000]}},
    {"s": [], "l": {}},
]


# ----------------------------------------------------------------- serial --

def find_port():
    ports = list(serial.tools.list_ports.comports())
    for p in ports:
        if (p.vid, p.pid) in ((0x1A86, 0x7523), (0x1A86, 0x55D4), (0x10C4, 0xEA60)):
            return p.device
    return None


def open_port(port):
    ser = serial.Serial()
    ser.port, ser.baudrate, ser.timeout = port, BAUD, 0
    ser.dtr = False  # don't toggle the auto-reset circuit when opening
    ser.rts = False
    ser.open()
    if hasattr(ser, "set_buffer_size"):  # Windows
        ser.set_buffer_size(rx_size=1 << 20)
    return ser


def send(ser, obj):
    ser.write((json.dumps(obj, separators=(",", ":"), ensure_ascii=False) + "\n").encode("utf-8"))


def screenshot(ser, path, payload):
    from PIL import Image  # only needed for this mode

    time.sleep(1.5)
    ser.reset_input_buffer()
    send(ser, payload)
    time.sleep(0.4)
    send(ser, {"cmd": "shot"})
    ser.timeout = 10
    buf = b""
    deadline = time.time() + 15
    while b"SHOT " not in buf and time.time() < deadline:
        buf += ser.read(max(1, ser.in_waiting))
    i = buf.index(b"SHOT ")
    header_end = buf.index(b"\n", i)
    w, h = map(int, buf[i + 5:header_end].split())
    data = buf[header_end + 1:]
    ser.timeout = 3
    while len(data) < w * h * 2:
        more = ser.read(w * h * 2 - len(data))
        if not more:
            raise SystemExit(f"screenshot incomplete: got {len(data)} of {w * h * 2} bytes")
        data += more
    img = Image.frombytes("RGB", (w, h), bytes(
        b for px in range(w * h)
        for b in _rgb565(data[2 * px] << 8 | data[2 * px + 1])))
    img.resize((w * 2, h * 2), Image.NEAREST).save(path)
    print(f"saved {path}")


def _rgb565(v):
    r, g, b = v >> 11, (v >> 5) & 63, v & 31
    return (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)


def run(args):
    tracker = Tracker()
    tracker.read_new(replay=True)
    ser, last_sent, last_payload, demo_i = None, 0.0, None, 0
    next_usage = 0.0
    while True:
        if not args.demo and time.time() >= next_usage:
            next_usage = time.time() + USAGE_EVERY
            try:
                fetch_usage()
            except Exception as e:  # keep whatever the status line last gave us
                print(f"usage check failed: {type(e).__name__}: {e}", flush=True)
        if ser is None:
            port = args.port or find_port()
            try:
                if not port:
                    raise serial.SerialException("no ESP32 serial port found")
                ser = open_port(port)
                print(f"connected on {port}", flush=True)
                last_payload = None
            except serial.SerialException as e:
                print(f"waiting for display: {e}", flush=True)
                time.sleep(3)
                continue

        now = time.time()
        if args.demo:
            payload = dict(DEMO[int(now / 6) % len(DEMO)], t=datetime.now().strftime("%H:%M"))
        else:
            tracker.read_new()
            tracker.prune(now)
            payload = build_payload(tracker, now)

        # Resend on change, or once a second as a heartbeat (ages are relative,
        # so comparing without them avoids resending every tick).
        key = json.dumps({**payload, "s": [{**s, "a": 0} for s in payload["s"]]}, sort_keys=True)
        if key != last_payload or now - last_sent >= 1.0:
            try:
                send(ser, payload)
                ser.read(ser.in_waiting or 0)  # discard anything the board printed
            except serial.SerialException as e:
                print(f"lost display: {e}", flush=True)
                ser.close()
                ser = None
                continue
            last_payload, last_sent = key, now
        time.sleep(0.15)


def setup_logging():
    if sys.stdout is None:  # pythonw (autostart) has no console
        os.makedirs(STATE_DIR, exist_ok=True)
        path = os.path.join(STATE_DIR, "bridge.log")
        if os.path.exists(path) and os.path.getsize(path) > 1 << 20:
            os.replace(path, path + ".old")
        sys.stdout = sys.stderr = open(path, "a", encoding="utf-8", buffering=1)
    print(f"--- bridge started {datetime.now():%Y-%m-%d %H:%M:%S}")


def main():
    setup_logging()
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port")
    ap.add_argument("--demo", action="store_true")
    ap.add_argument("--shot", metavar="PNG")
    ap.add_argument("--demo-frame", type=int, default=0, help="with --shot: which demo state to show")
    args = ap.parse_args()

    if args.shot:
        ser = open_port(args.port or find_port())
        if args.demo:
            payload = dict(DEMO[args.demo_frame % len(DEMO)], t=datetime.now().strftime("%H:%M"))
        else:
            tracker = Tracker()
            tracker.read_new(replay=True)
            payload = build_payload(tracker, time.time())
        screenshot(ser, args.shot, payload)
        return
    while True:
        try:
            run(args)
        except KeyboardInterrupt:
            return
        except Exception:
            import traceback
            traceback.print_exc()
            time.sleep(3)


if __name__ == "__main__":
    sys.exit(main())
