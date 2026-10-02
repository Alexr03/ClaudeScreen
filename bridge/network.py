"""WiFi side of the bridge: find screens, pair with them, keep them updated.

Screens announce `_claudescreen._tcp` over mDNS. This PC talks only to the
screens it has paired with; their keys live in ~/.claude/claude-screen/screens.json.
See firmware/src/net.h for the protocol.
"""
import getpass
import hashlib
import hmac
import json
import os
import socket
import threading
import time

STATE_DIR = os.path.join(os.path.expanduser("~"), ".claude", "claude-screen")
SCREENS = os.path.join(STATE_DIR, "screens.json")
SERVICE = "_claudescreen._tcp.local."


def label():
    """How this PC names itself to screens, e.g. alex@DESKTOP-1."""
    return f"{getpass.getuser()}@{socket.gethostname()}"[:31]


def load_screens():
    try:
        with open(SCREENS, encoding="utf-8") as f:
            return json.load(f).get("screens", {})
    except (OSError, ValueError):
        return {}


def save_screens(screens):
    os.makedirs(STATE_DIR, mode=0o700, exist_ok=True)
    tmp = SCREENS + ".tmp"
    # Holds pairing keys: owner-only on POSIX (Windows profiles are private already).
    fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump({"screens": screens}, f, indent=2)
    os.replace(tmp, SCREENS)


# ------------------------------------------------------------- discovery --

class Discovery:
    """Background mDNS browser: id -> {"id", "name", "host", "port"}."""

    def __init__(self):
        from zeroconf import ServiceBrowser, Zeroconf

        self.found = {}
        self.lock = threading.Lock()
        self.zc = Zeroconf()
        self.browser = ServiceBrowser(self.zc, SERVICE, self)

    def _resolve(self, zc, type_, name):
        info = zc.get_service_info(type_, name, timeout=3000)
        if not info or not info.parsed_addresses():
            return
        props = {k.decode(): (v or b"").decode() for k, v in info.properties.items()}
        sid = props.get("id") or name.split(".")[0]
        with self.lock:
            self.found[sid] = {"id": sid, "name": props.get("name", sid),
                               "host": info.parsed_addresses()[0], "port": info.port, "service": name}

    def add_service(self, zc, type_, name):
        self._resolve(zc, type_, name)

    update_service = add_service

    def remove_service(self, zc, type_, name):
        with self.lock:
            for sid, s in list(self.found.items()):
                if s["service"] == name:
                    del self.found[sid]

    def screens(self):
        with self.lock:
            return dict(self.found)

    def close(self):
        self.zc.close()


# ------------------------------------------------------------ connection --

def _connect(host, port):
    import websocket

    ws = websocket.create_connection(f"ws://{host}:{port}/", timeout=5)
    hello = json.loads(ws.recv())
    if hello.get("hello") != "claudescreen":
        ws.close()
        raise ConnectionError("not a ClaudeScreen")
    return ws, hello


class ScreenLink:
    """One authenticated connection to a paired screen, re-established on demand."""

    def __init__(self, sid, key_hex):
        self.sid = sid
        self.key = bytes.fromhex(key_hex)
        self.ws = None
        self.retry_at = 0.0
        self.name = sid

    def connect(self, where):
        ws, hello = _connect(where["host"], where["port"])
        nonce = bytes.fromhex(hello["nonce"])
        mac = hmac.new(self.key, nonce, hashlib.sha256).hexdigest()
        ws.send(json.dumps({"auth": label(), "mac": mac}))
        reply = json.loads(ws.recv())
        if not reply.get("ok"):
            ws.close()
            raise PermissionError(reply.get("err", "authentication failed"))
        self.name = reply.get("name", self.sid)
        self.ws = ws

    def send(self, text):
        self.ws.send(text)

    def close(self):
        if self.ws:
            try:
                self.ws.close()
            except Exception:
                pass
        self.ws = None


class Network:
    """Keeps a link to every paired screen that is on the network."""

    def __init__(self, log):
        self.log = log
        self.discovery = Discovery()
        self.links = {}
        self.screens_mtime = None
        self.screens = {}

    def _reload(self):
        try:
            mtime = os.path.getmtime(SCREENS)
        except OSError:
            mtime = None
        if mtime == self.screens_mtime:
            return
        self.screens_mtime = mtime
        self.screens = load_screens()
        for sid in list(self.links):
            if sid not in self.screens or self.links[sid].key.hex() != self.screens[sid]["key"]:
                self.links.pop(sid).close()

    def send(self, text):
        self._reload()
        found = self.discovery.screens()
        now = time.time()
        for sid, cfg in self.screens.items():
            link = self.links.setdefault(sid, ScreenLink(sid, cfg["key"]))
            if link.ws is None:
                if sid not in found or now < link.retry_at:
                    continue
                try:
                    link.connect(found[sid])
                    self.log(f"connected to {link.name} ({found[sid]['host']}) over WiFi")
                except PermissionError as e:
                    self.log(f"{cfg.get('name', sid)}: {e} - pair again with: claude_screen.py pair")
                    link.retry_at = now + 300
                    continue
                except Exception as e:
                    self.log(f"{cfg.get('name', sid)}: can't connect: {e}")
                    link.retry_at = now + 10
                    continue
            try:
                link.send(text)
            except Exception as e:
                self.log(f"lost {link.name}: {e}")
                link.close()
                link.retry_at = now + 2

    def close(self):
        for link in self.links.values():
            link.close()
        self.discovery.close()


# ---------------------------------------------------------------- pairing --

def browse(seconds=4.0):
    d = Discovery()
    time.sleep(seconds)
    found = d.screens()
    d.close()
    return found


def pair(target=None):
    print("Looking for screens on the network...")
    found = browse()
    paired = load_screens()
    if not found:
        print("No screens found. Is the screen on WiFi, and this PC on the same network?")
        return 1
    if target:
        found = {k: v for k, v in found.items() if target.lower() in (k + v["name"]).lower()}
        if not found:
            print(f"No screen matching '{target}'.")
            return 1
    items = sorted(found.values(), key=lambda s: s["name"])
    for i, s in enumerate(items, 1):
        mark = "  (paired)" if s["id"] in paired else ""
        print(f"  {i}. {s['name']}  -  {s['host']}{mark}")
    if len(items) == 1:
        screen = items[0]
    else:
        choice = input("Pair with which screen? [number] ").strip()
        if not choice.isdigit() or not 1 <= int(choice) <= len(items):
            print("Cancelled.")
            return 1
        screen = items[int(choice) - 1]

    ws, hello = _connect(screen["host"], screen["port"])
    me = label()
    ws.send(json.dumps({"pair": me}))
    reply = json.loads(ws.recv())
    if not reply.get("pairing"):
        print(f"Screen refused: {reply.get('err', reply)}")
        return 1
    nonce = bytes.fromhex(hello["nonce"])
    print(f"\n{screen['name']} is showing a 6-digit code.")
    for _ in range(3):
        code = "".join(ch for ch in input("Code: ") if ch.isdigit())
        proof = hmac.new(code.encode(), nonce + me.encode(), hashlib.sha256).hexdigest()
        ws.send(json.dumps({"pair_proof": proof}))
        reply = json.loads(ws.recv())
        if "paired" in reply:
            mask = hashlib.sha256(code.encode() + nonce + b"claudescreen-key").digest()
            key = bytes(a ^ b for a, b in zip(bytes.fromhex(reply["paired"]), mask))
            paired[screen["id"]] = {"name": reply.get("name", screen["name"]), "key": key.hex()}
            save_screens(paired)
            ws.close()
            print(f"Paired with {paired[screen['id']]['name']}. The bridge will start sending to it.")
            return 0
        print(reply.get("err", "failed"))
        if "cancelled" in reply.get("err", ""):
            break
    ws.close()
    return 1


def list_screens():
    found = browse()
    paired = load_screens()
    print(f"This PC: {label()}\n")
    for sid in sorted(set(found) | set(paired)):
        f, p = found.get(sid), paired.get(sid)
        name = (f or p)["name"]
        state = ("paired" if p else "not paired") + (f", online at {f['host']}" if f else ", not found")
        print(f"  {name}  [{sid}]  {state}")
    if not found and not paired:
        print("  No screens found.")
    return 0


def unpair(target):
    paired = load_screens()
    hits = [sid for sid, s in paired.items() if target.lower() in (sid + s["name"]).lower()]
    if len(hits) != 1:
        print("Paired screens:", ", ".join(f"{s['name']} [{sid}]" for sid, s in paired.items()) or "none")
        return 1
    sid = hits[0]
    found = browse(3)
    if sid in found:  # also tell the screen to forget us
        link = ScreenLink(sid, paired[sid]["key"])
        try:
            link.connect(found[sid])
            link.send(json.dumps({"cmd": "unpair"}))
        except Exception:
            pass
        link.close()
    print(f"Unpaired {paired.pop(sid)['name']}.")
    save_screens(paired)
    return 0
