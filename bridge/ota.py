"""Firmware updates over WiFi, for screens this PC is paired with.

    python claude_screen.py update                     latest GitHub release
    python claude_screen.py update firmware.bin        a local build (app image)
    python claude_screen.py update --screen kitchen    pick a screen by name

The bridge asks the screen (over its authenticated link) to accept one update
with a one-time password, then sends the image with the standard ArduinoOTA
protocol: a UDP invitation, digest auth, and the screen downloading the image
from a TCP port opened here. Windows may ask once to let Python accept that
connection.
"""
import hashlib
import json
import os
import secrets
import socket
import sys
import tempfile
import urllib.request

import network

REPO = "Alexr03/ClaudeScreen"
OTA_PORT = 3232


def latest_release_app():
    req = urllib.request.Request(f"https://api.github.com/repos/{REPO}/releases/latest",
                                 headers={"Accept": "application/vnd.github+json", "User-Agent": "claude-screen"})
    with urllib.request.urlopen(req, timeout=15) as r:
        rel = json.load(r)
    asset = next((a for a in rel["assets"] if a["name"].endswith("-app.bin")), None)
    if not asset:
        raise SystemExit(f"release {rel['tag_name']} has no app image")
    path = os.path.join(tempfile.gettempdir(), asset["name"])
    print(f"Downloading {rel['tag_name']} ({asset['size'] // 1024} KB)...")
    urllib.request.urlretrieve(asset["browser_download_url"], path)
    return path


def local_ip_towards(host):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect((host, OTA_PORT))
        return s.getsockname()[0]
    finally:
        s.close()


def push(host, password, image):
    """ArduinoOTA upload; returns once the screen confirms the new image."""
    data = open(image, "rb").read()
    md5 = hashlib.md5(data).hexdigest()
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.bind((local_ip_towards(host), 0))
    srv.listen(1)
    srv.settimeout(15)
    port = srv.getsockname()[1]

    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.settimeout(5)
    udp.sendto(f"0 {port} {len(data)} {md5}\n".encode(), (host, OTA_PORT))
    reply = udp.recv(64).decode()
    if reply.startswith("AUTH"):
        nonce = reply.split()[1]
        cnonce = hashlib.md5(f"{os.path.basename(image)}{len(data)}{md5}{host}".encode()).hexdigest()
        result = hashlib.md5(f"{hashlib.md5(password.encode()).hexdigest()}:{nonce}:{cnonce}".encode()).hexdigest()
        udp.sendto(f"200 {cnonce} {result}\n".encode(), (host, OTA_PORT))
        reply = udp.recv(64).decode()
    udp.close()
    if "OK" not in reply:
        raise SystemExit(f"screen refused the update: {reply.strip()}")

    try:
        conn, _ = srv.accept()
    except socket.timeout:
        raise SystemExit("the screen never connected back - is a firewall blocking Python?")
    conn.settimeout(20)
    sent, shown = 0, -1
    while sent < len(data):
        chunk = data[sent:sent + 1024]
        conn.sendall(chunk)
        sent += len(chunk)
        conn.recv(16)  # the screen acks each chunk
        pct = sent * 100 // len(data)
        if pct != shown:
            shown = pct
            sys.stdout.write(f"\r  uploading {pct:3d}%")
            sys.stdout.flush()
    print()
    conn.settimeout(60)
    result = b""
    while b"OK" not in result:
        more = conn.recv(32)
        if not more:
            break
        result += more
    conn.close()
    srv.close()
    if b"OK" not in result:
        raise SystemExit(f"update failed: {result.decode(errors='replace').strip()}")


def update(image=None, target=None):
    paired = network.load_screens()
    if not paired:
        print("No paired screens. Pair first: claude_screen.py pair")
        return 1
    found = network.browse()
    candidates = {sid: s for sid, s in paired.items() if sid in found
                  and (not target or target.lower() in (sid + s["name"]).lower())}
    if not candidates:
        print("None of your paired screens are on the network right now.")
        return 1
    image = image or latest_release_app()

    for sid, cfg in candidates.items():
        where = found[sid]
        print(f"Updating {cfg['name']} at {where['host']}...")
        link = network.ScreenLink(sid, cfg["key"])
        link.connect(where)
        password = secrets.token_hex(16)
        link.send(json.dumps({"cmd": "ota", "pw": password}))
        reply = json.loads(link.ws.recv())
        link.close()
        if "ota" not in reply:
            print(f"  screen refused: {reply.get('err', reply)} (its firmware may predate WiFi updates)")
            continue
        push(where["host"], password, image)
        print("  done - the screen restarts on the new firmware.")
    return 0
