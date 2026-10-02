"""Change settings stored on the board, over USB (stop the bridge first).

    python display_config.py                              show current settings
    python display_config.py wifi_ssid=Home wifi_pass=... join a WiFi network
    python display_config.py forget_wifi=1                forget WiFi
    python display_config.py name="Alex's screen"         name shown when pairing
    python display_config.py otapw=...                    enable updates over WiFi
    python display_config.py unpair                       forget every paired PC
    python display_config.py inv=1                        colours: 0 auto, 1 normal, 2 inverted
    python display_config.py panel=2                      driver: 0 auto, 1 ILI9341, 2 ST7789
    python display_config.py rot=3                        rotation 0-3 (1/3 = landscape)
    python display_config.py bgr=1                        swap red/blue
"""
import json
import sys
import time

from claude_screen import find_port, open_port

ser = open_port(find_port())
time.sleep(0.3)
ser.read(ser.in_waiting or 0)

args = sys.argv[1:]
if args == ["unpair"]:
    msg = {"cmd": "unpair"}
else:
    settings = {}
    for a in args:
        k, v = a.split("=", 1)
        settings[k] = int(v) if v.isdigit() and k not in ("wifi_ssid", "wifi_pass", "name", "otapw") else v
    msg = {"cmd": "set", **settings} if settings else {"cmd": "ping"}

ser.write((json.dumps(msg) + "\n").encode())
time.sleep(0.5 if msg["cmd"] == "ping" else 3)
if msg["cmd"] != "ping":  # the board may have rebooted; ask again for the final state
    if "wifi_ssid" in msg:
        time.sleep(5)  # give it a moment to join
    ser.read(ser.in_waiting or 0)
    ser.write(b'{"cmd":"ping"}\n')
    time.sleep(0.5)
lines = [l for l in ser.read(ser.in_waiting or 0).decode("utf-8", "replace").splitlines() if l.startswith("PONG")]
print(lines[-1][len("PONG claudescreen 2 "):] if lines else "no answer from the screen")
