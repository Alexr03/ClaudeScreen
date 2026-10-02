"""Change display settings stored on the board (stop the bridge first).

    python display_config.py                      show current settings
    python display_config.py inv=1                colours: 0 auto, 1 normal, 2 inverted
    python display_config.py panel=2              driver: 0 auto, 1 ILI9341, 2 ST7789
    python display_config.py rot=3                rotation 0-3 (1/3 = landscape)
    python display_config.py bgr=1                swap red/blue
"""
import json
import sys
import time

from claude_screen import find_port, open_port

ser = open_port(find_port())
time.sleep(0.3)
ser.read(ser.in_waiting or 0)
settings = {k: int(v) for k, v in (a.split("=", 1) for a in sys.argv[1:])}
ser.write((json.dumps({"cmd": "set", **settings} if settings else {"cmd": "ping"}) + "\n").encode())
time.sleep(3 if settings else 0.5)
if settings:  # board reboots on driver changes; ask again for the final state
    ser.read(ser.in_waiting or 0)
    ser.write(b'{"cmd":"ping"}\n')
    time.sleep(0.5)
print(ser.read(ser.in_waiting or 0).decode("utf-8", "replace").strip().splitlines()[-1])
