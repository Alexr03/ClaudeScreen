# ClaudeScreen

Claude Code activity and plan usage on an ESP32-2432S028 ("Cheap Yellow Display"), over USB.

- **Hero card**: what the most urgent session is doing (Thinking / Editing / Running / Needs you / Done), the current tool and file or command, time in that state, and the context window.
- **Chips**: every active session. Amber = waiting on you, orange = working, green = done.
- **Rings**: 5-hour and weekly usage limits, with time until each resets.
- **Tap**: cycle through sessions. **Long press**: rotate 180°.
- The backlight dims after 10 minutes of no activity, and the LED on the back pulses amber while Claude is waiting for you.

## How it works

```
Claude Code hooks ──> bridge/hook.py ──────> ~/.claude/claude-screen/events.jsonl ─┐
Claude Code status line ──> bridge/statusline.py ──> status-*.json, rate_limits.json ┼─> bridge/claude_screen.py ──USB──> ESP32
```

`install.py` adds async hooks to `~/.claude/settings.json` (backed up first), puts the status line tap in front of your existing status line, and adds `ClaudeScreen.vbs` to your Startup folder so the bridge runs at login. `python bridge/install.py --remove` undoes all of it.

Usage limits come from the status line, so they update only while a terminal (CLI) Claude Code session is running.

## Commands

```bash
python bridge/install.py                 # hook into Claude Code + autostart
python bridge/claude_screen.py --demo    # cycle demo screens
python bridge/claude_screen.py --shot s.png   # screenshot the display's framebuffer
python bridge/display_config.py inv=1    # panel settings (stop the bridge first)
```

The bridge logs to `~/.claude/claude-screen/bridge.log`.

## Firmware

PlatformIO project in `firmware/`. Fonts are rasterised from Roboto and JetBrains Mono by `tools/make_fonts.py`.

```bash
cd firmware && pio run -t upload --upload-port COM4
```

This board doesn't enter flashing mode by itself: hold **BOOT** while the upload starts.

The display settings are stored on the board, not in the firmware. This unit uses `panel=2 inv=1` (ST7789, no inversion). If the flash is ever erased, run `python bridge/display_config.py panel=2 inv=1` again.
