// ClaudeScreen - Claude Code activity + plan usage on an ESP32-2432S028.
//
// The PC bridge (bridge/claude_screen.py) streams one JSON line per second:
//   {"t":"14:32","s":[{"n":"proj","st":"work","v":"Editing","d":"main.cpp",
//    "a":42,"c":31,"m":"Opus 5.5"}],"l":{"h5":[23.5,8040],"d7":[41,300000]}}
// st: work | wait | idle.  a: seconds in this state.  c: context % (-1 unknown).
// l.*: [used %, seconds until reset]; a missing window means "unknown".
//
// The frame is rendered in 40px bands into two ping-pong sprites so a full
// 16-bit frame never has to fit in RAM, and each band's DMA transfer overlaps
// with rendering the next one.

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <math.h>

#include "board.h"
#include "fonts.h"

// ---------------------------------------------------------------- palette --

static constexpr uint16_t rgb(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static constexpr uint16_t C_BG = rgb(0x14, 0x14, 0x13);
static constexpr uint16_t C_CARD = rgb(0x21, 0x20, 0x1D);
static constexpr uint16_t C_TRACK = rgb(0x33, 0x31, 0x2C);
static constexpr uint16_t C_TEXT = rgb(0xF0, 0xEE, 0xE6);
static constexpr uint16_t C_MUTED = rgb(0xA3, 0x9E, 0x92);
static constexpr uint16_t C_DIM = rgb(0x6E, 0x6A, 0x61);
static constexpr uint16_t C_CORAL = rgb(0xD9, 0x77, 0x57);
static constexpr uint16_t C_AMBER = rgb(0xEA, 0xB3, 0x4E);
static constexpr uint16_t C_GREEN = rgb(0x86, 0xBF, 0x84);
static constexpr uint16_t C_RED = rgb(0xE5, 0x5F, 0x4C);

// ----------------------------------------------------------------- state --

struct Session {
  char name[40];
  char state;  // 'w' working, 'q' waiting on user, 'i' idle
  char verb[24];
  char detail[96];
  uint32_t age;
  int ctx;
  char model[24];
};

struct Limit {
  bool known = false;
  float pct = 0;
  int32_t reset = 0;  // seconds, relative to rxMs
};

static constexpr int MAX_SESS = 6;
static Session sess[MAX_SESS];
static int nSess = 0;
static Limit lim5h, lim7d;
static char clockStr[8] = "";
static uint32_t rxMs = 0;  // when the last update arrived
static bool everConnected = false;

static int focus = 0;             // which session the hero card shows
static uint32_t manualFocusMs = 0;  // non-zero while the user picked one by tapping

// ------------------------------------------------------------- hardware --

static LGFX lcd;
static LGFX_Sprite band[2] = {LGFX_Sprite(&lcd), LGFX_Sprite(&lcd)};
static constexpr int W = 320, H = 240, BH = 40;
static LGFX_Sprite* S;  // band being drawn
static int OY;          // its top edge in screen coordinates
static Preferences prefs;
static uint8_t rotation = 1;

static lgfx::VLWfont fHero, fTitle, fNum, fBody, fSmall, fMono;
static lgfx::PointerWrapper wHero, wTitle, wNum, wBody, wSmall, wMono;

static void loadFont(lgfx::VLWfont& f, lgfx::PointerWrapper& w, const uint8_t* data) {
  w.set(data);
  f.loadFont(&w);
}

// --------------------------------------------------------------- drawing --
// All coordinates are screen coordinates; helpers translate into the band.

static inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

static uint16_t mix(uint16_t a, uint16_t b, float t) {
  int ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31;
  int br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
  int r = ar + (int)((br - ar) * t + 0.5f);
  int g = ag + (int)((bg - ag) * t + 0.5f);
  int bl = ab + (int)((bb - ab) * t + 0.5f);
  return (r << 11) | (g << 5) | bl;
}

static inline void blendPx(int x, int y, uint16_t col, float a) {
  if (x < 0 || x >= W || y < OY || y >= OY + BH || a <= 0) return;
  uint16_t* p = (uint16_t*)S->getBuffer() + (y - OY) * W + x;
  if (a >= 1) {
    *p = __builtin_bswap16(col);
  } else {
    *p = __builtin_bswap16(mix(__builtin_bswap16(*p), col, a));
  }
}

// Paint `col` wherever the signed distance field is negative, with 1px AA.
template <typename F>
static void paintSDF(int x0, int y0, int x1, int y1, uint16_t col, float alpha, F sdf) {
  if (y1 < OY || y0 >= OY + BH) return;
  y0 = max(y0, OY);
  y1 = min(y1, OY + BH - 1);
  x0 = max(x0, 0);
  x1 = min(x1, W - 1);
  for (int y = y0; y <= y1; y++)
    for (int x = x0; x <= x1; x++) {
      float d = sdf(x + 0.5f, y + 0.5f);
      if (d < 0.5f) blendPx(x, y, col, clamp01(0.5f - d) * alpha);
    }
}

static inline float segDist(float px, float py, float ax, float ay, float bx, float by) {
  float pax = px - ax, pay = py - ay, bax = bx - ax, bay = by - ay;
  float l2 = bax * bax + bay * bay;
  float h = l2 > 0 ? clamp01((pax * bax + pay * bay) / l2) : 0;
  float dx = pax - bax * h, dy = pay - bay * h;
  return sqrtf(dx * dx + dy * dy);
}

static void rrect(int x, int y, int w, int h, int r, uint16_t col) {
  if (y + h < OY || y >= OY + BH) return;
  S->fillSmoothRoundRect(x, y - OY, w, h, r, col);
}

static void disc(float cx, float cy, float r, uint16_t col, float alpha = 1) {
  paintSDF(cx - r - 1, cy - r - 1, cx + r + 1, cy + r + 1, col, alpha, [&](float x, float y) {
    return sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy)) - r;
  });
}

static void capsule(float ax, float ay, float bx, float by, float r, uint16_t col, float alpha = 1) {
  paintSDF(min(ax, bx) - r - 1, min(ay, by) - r - 1, max(ax, bx) + r + 1, max(ay, by) + r + 1, col, alpha,
           [&](float x, float y) { return segDist(x, y, ax, ay, bx, by) - r; });
}

// Ring arc with rounded caps, clockwise from 12 o'clock, `frac` of a turn.
static void arc(float cx, float cy, float rm, float hw, float frac, uint16_t col) {
  if (frac <= 0) return;
  frac = min(frac, 1.0f);
  float sweep = frac * 2 * PI;
  float ex = cx + rm * sinf(sweep), ey = cy - rm * cosf(sweep);
  float sx = cx, sy = cy - rm;
  int R = rm + hw + 1;
  paintSDF(cx - R, cy - R, cx + R, cy + R, col, 1, [&](float x, float y) {
    float dx = x - cx, dy = y - cy;
    float d = sqrtf(dx * dx + dy * dy);
    float th = atan2f(dx, -dy);
    if (th < 0) th += 2 * PI;
    if (frac >= 1 || th <= sweep) return fabsf(d - rm) - hw;
    float d1 = sqrtf((x - sx) * (x - sx) + (y - sy) * (y - sy));
    float d2 = sqrtf((x - ex) * (x - ex) + (y - ey) * (y - ey));
    return min(d1, d2) - hw;
  });
}

static void glow(float cx, float cy, float r, uint16_t col, float strength) {
  paintSDF(cx - r, cy - r, cx + r, cy + r, col, 1, [&](float x, float y) {
    float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy)) / r;
    float a = d >= 1 ? 0 : (1 - d) * (1 - d) * strength;
    return 0.5f - a;  // paintSDF turns this back into alpha `a`
  });
}

// The Claude spark: twelve rays of uneven length. `t` drives the animation;
// `breathe` scales how much each ray pulses (0 = static).
static void spark(float cx, float cy, float R, uint16_t col, float t, float spin, float breathe) {
  static const float len[12] = {1.0, .78, .92, .70, .96, .80, 1.0, .74, .90, .76, .94, .72};
  float w = max(1.0f, R * 0.105f);
  for (int i = 0; i < 12; i++) {
    float a = i * (2 * PI / 12) + spin;
    float l = R * len[i] * (1 - breathe * 0.5f * (1 + sinf(t * 5.0f - i * 0.9f)));
    float r0 = R * 0.12f;
    capsule(cx + r0 * sinf(a), cy - r0 * cosf(a), cx + l * sinf(a), cy - l * cosf(a), w, col);
  }
  disc(cx, cy, w * 1.6f, col);
}

static void check(float cx, float cy, float R, uint16_t col) {
  disc(cx, cy, R, col, 0.16f);
  float w = R * 0.11f;
  capsule(cx - R * 0.42f, cy + R * 0.02f, cx - R * 0.12f, cy + R * 0.32f, w, col);
  capsule(cx - R * 0.12f, cy + R * 0.32f, cx + R * 0.45f, cy - R * 0.30f, w, col);
}

static void text(const lgfx::IFont& f, const char* s, int x, int baseline, uint16_t col,
                 textdatum_t datum = textdatum_t::baseline_left) {
  if (baseline + 12 < OY || baseline - 34 >= OY + BH) return;
  S->setFont(&f);
  S->setTextColor(col);
  S->setTextDatum(datum);
  S->drawString(s, x, baseline - OY);
}

static int textW(const lgfx::IFont& f, const char* s) {
  S->setFont(&f);
  return S->textWidth(s);
}

// Copy `s` into `out`, cutting it with an ellipsis so it fits `maxw` pixels.
static const char* fit(const lgfx::IFont& f, const char* s, int maxw, char* out, size_t cap) {
  strlcpy(out, s, cap);
  if (textW(f, out) <= maxw) return out;
  size_t n = strlen(out);
  while (n > 0) {
    n--;
    while (n > 0 && (out[n] & 0xC0) == 0x80) n--;  // stay on a UTF-8 boundary
    strcpy(out + n, "\xE2\x80\xA6");
    if (textW(f, out) <= maxw) break;
  }
  return out;
}

// ------------------------------------------------------------ formatting --

static void fmtDur(uint32_t s, char* out, size_t cap, bool secs) {
  if (s < 60) snprintf(out, cap, secs ? "%us" : "<1m", s);
  else if (s < 3600) {
    if (secs) snprintf(out, cap, "%um %02us", s / 60, s % 60);
    else snprintf(out, cap, "%um", s / 60);
  } else if (s < 86400) snprintf(out, cap, "%uh %02um", s / 3600, (s % 3600) / 60);
  else snprintf(out, cap, "%ud %uh", s / 86400, (s % 86400) / 3600);
}

static uint16_t stateColor(char st) {
  switch (st) {
    case 'w': return C_CORAL;
    case 'q': return C_AMBER;
    default: return C_GREEN;
  }
}

static uint16_t usageColor(float pct) {
  if (pct >= 90) return C_RED;
  if (pct >= 75) return C_AMBER;
  return C_CORAL;
}

// ------------------------------------------------------------------ scene --

static float T;  // animation clock, seconds
static bool online() { return everConnected && millis() - rxMs < 6000; }
static uint32_t sinceRx() { return (millis() - rxMs) / 1000; }

static void drawHeader() {
  bool working = online() && nSess > 0 && sess[focus].state == 'w';
  spark(20, 17, 10, online() ? C_CORAL : C_DIM, T, working ? T * 0.8f : 0, 0);
  text(fTitle, "Claude", 37, 23, C_TEXT);

  const char* model = (online() && nSess > 0) ? sess[focus].model : "";
  int x = 37 + textW(fTitle, "Claude") + 8;
  if (*model) {
    int pw = textW(fSmall, model) + 16;
    rrect(x, 8, pw, 19, 9, C_CARD);
    text(fSmall, model, x + 8, 22, C_MUTED);
  }
  if (online()) text(fTitle, clockStr, 312, 23, C_MUTED, textdatum_t::baseline_right);
}

static void drawHero() {
  const int cx = 8, cy = 34, cw = 304, ch = 100;
  rrect(cx, cy, cw, ch, 12, C_CARD);
  const float ox = 50, oy = cy + ch / 2;
  const int tx = 94, tr = cx + cw - 12, tw = tr - tx;
  char buf[112], line[112];

  if (!online()) {
    spark(ox, oy, 24, C_DIM, T, 0, 0);
    text(fSmall, everConnected ? "CONNECTION LOST" : "CLAUDESCREEN", tx, 56, C_DIM);
    text(fHero, "Waiting for PC", tx, 88, C_MUTED);
    text(fMono, "run claude_screen.py", tx, 108, C_DIM);
    return;
  }
  if (nSess == 0) {
    glow(ox, oy, 42, C_CORAL, 0.18f);
    spark(ox, oy, 24, C_CORAL, T, 0, 0);
    text(fSmall, "CLAUDE CODE", tx, 56, C_MUTED);
    text(fHero, "All quiet", tx, 88, C_TEXT);
    text(fMono, "no active sessions", tx, 108, C_DIM);
    return;
  }

  const Session& s = sess[focus];
  uint16_t col = stateColor(s.state);
  glow(ox, oy, 44, col, s.state == 'q' ? 0.16f + 0.10f * sinf(T * 4) : 0.18f);
  if (s.state == 'w') {
    spark(ox, oy, 25, col, T, T * 0.35f, 0.32f);
  } else if (s.state == 'q') {
    float p = fmodf(T * 0.9f, 1.0f);  // expanding "ping" ring
    float rp = 24 + p * 16;
    paintSDF(ox - 42, oy - 42, ox + 42, oy + 42, col, (1 - p) * 0.7f, [&](float x, float y) {
      return fabsf(sqrtf((x - ox) * (x - ox) + (y - oy) * (y - oy)) - rp) - 1.2f;
    });
    spark(ox, oy, 25, col, T, 0, 0);
  } else {
    check(ox, oy, 24, col);
  }

  // Line 1: project (+ counter), time in this state on the right.
  char age[16];
  fmtDur(s.age + sinceRx(), age, sizeof age, true);
  int ageW = textW(fMono, age);
  text(fMono, age, tr, 54, C_DIM, textdatum_t::baseline_right);
  if (nSess > 1) snprintf(buf, sizeof buf, "%s  \xC2\xB7  %d of %d", s.name, focus + 1, nSess);
  else strlcpy(buf, s.name, sizeof buf);
  text(fSmall, fit(fSmall, buf, tw - ageW - 10, line, sizeof line), tx, 54, C_MUTED);

  // Line 2: the big verb. Line 3: detail.
  text(fHero, fit(fHero, s.verb, tw, line, sizeof line), tx, 85, col);
  if (*s.detail) text(fMono, fit(fMono, s.detail, tw, line, sizeof line), tx, 104, C_TEXT);

  // Line 4: context window bar.
  if (s.ctx >= 0) {
    const int by = 121;
    text(fSmall, "Context", tx, by + 4, C_DIM);
    snprintf(buf, sizeof buf, "%d%%", s.ctx);
    text(fSmall, buf, tr, by + 4, C_MUTED, textdatum_t::baseline_right);
    int bx = tx + textW(fSmall, "Context") + 8, bw = tr - 34 - bx;
    capsule(bx, by, bx + bw, by, 1.6f, C_TRACK);
    float f = clamp01(s.ctx / 100.0f);
    if (f > 0) capsule(bx, by, bx + bw * f, by, 1.6f, s.ctx >= 80 ? C_AMBER : C_MUTED);
  }
}

static void drawChips() {
  if (!online()) return;
  int x = 8;
  const int y = 140, h = 19;
  char nm[24];
  for (int i = 0; i < nSess; i++) {
    fit(fSmall, sess[i].name, 90, nm, sizeof nm);
    int w = textW(fSmall, nm) + 27;
    if (x + w > 312) {
      char more[8];
      snprintf(more, sizeof more, "+%d", nSess - i);
      text(fSmall, more, x + 2, y + 14, C_DIM);
      break;
    }
    bool f = i == focus;
    rrect(x, y, w, h, 9, f ? C_TRACK : C_CARD);
    uint16_t col = stateColor(sess[i].state);
    float a = sess[i].state == 'q' ? 0.55f + 0.45f * sinf(T * 5) : 1;
    disc(x + 11, y + h / 2.0f, 3.2f, col, a);
    text(fSmall, nm, x + 19, y + 14, f ? C_TEXT : C_MUTED);
    x += w + 6;
  }
}

static void drawTile(int x, const char* ringLabel, const char* label, const Limit& l) {
  const int y = 165, w = 148, h = 71;
  rrect(x, y, w, h, 12, C_CARD);
  const float rx = x + 34, ry = y + h / 2.0f;
  arc(rx, ry, 22, 3.6f, 1, C_TRACK);

  char pct[12], rs[32], d[16];
  int32_t left = l.known ? l.reset - (int32_t)sinceRx() : 0;
  bool expired = l.known && left <= 0;  // window rolled over since we heard
  float p = 0;
  if (l.known) {
    p = expired ? 0 : l.pct;
    arc(rx, ry, 22, 3.6f, p / 100.0f, usageColor(p));
    snprintf(pct, sizeof pct, "%d%%", (int)(p + 0.5f));
    if (expired) strlcpy(rs, "window reset", sizeof rs);
    else {
      fmtDur(left, d, sizeof d, false);
      snprintf(rs, sizeof rs, "resets %s", d);
    }
  } else {
    strlcpy(pct, "\xE2\x80\x94", sizeof pct);
    strlcpy(rs, "waiting for data", sizeof rs);
  }
  text(fTitle, ringLabel, rx, ry + 6, C_TEXT, textdatum_t::baseline_center);
  const int tx = x + 68;
  text(fSmall, label, tx, y + 20, C_MUTED);
  text(fHero, pct, tx - 1, y + 48, l.known ? (p >= 75 ? usageColor(p) : C_TEXT) : C_DIM);
  text(fSmall, rs, tx, y + 63, C_DIM);
}

static void drawScene() {
  S->fillSprite(C_BG);
  drawHeader();
  drawHero();
  drawChips();
  drawTile(8, "5h", "Session", lim5h);
  drawTile(164, "7d", "Weekly", lim7d);
}

// ----------------------------------------------------------------- render --

static bool shotRequested = false;
static void printInfo();

static void renderFrame() {
  bool shot = shotRequested;
  shotRequested = false;
  if (shot) Serial.printf("SHOT %d %d\n", W, H);
  lcd.startWrite();
  for (int b = 0; b < H / BH; b++) {
    S = &band[b & 1];
    OY = b * BH;
    drawScene();
    lcd.waitDMA();
    if (shot) {
      // Paced: the CH340 driver on Windows drops bytes at full 921600 speed.
      const uint8_t* p = (const uint8_t*)S->getBuffer();
      for (int i = 0; i < W * BH * 2; i += 256) {
        Serial.write(p + i, 256);
        Serial.flush();
        delayMicroseconds(1500);
      }
    }
    lcd.pushImageDMA(0, OY, W, BH, (lgfx::swap565_t*)S->getBuffer());
  }
  lcd.waitDMA();
  lcd.endWrite();
  if (shot) Serial.flush();
}

// ----------------------------------------------------------------- serial --

static void parseLine(char* line) {
  JsonDocument doc;
  if (deserializeJson(doc, line)) return;

  if (doc["cmd"].is<const char*>()) {
    if (!strcmp(doc["cmd"], "shot")) shotRequested = true;
    if (!strcmp(doc["cmd"], "ping")) printInfo();
    // Display settings, stored in flash. "panel": 0 auto, 1 ILI9341, 2 ST7789;
    // "inv": 0 auto, 1 off, 2 on; "bgr": 0/1; "rot": 0-3. Panel changes reboot.
    if (!strcmp(doc["cmd"], "set")) {
      bool reboot = false;
      for (const char* k : {"panel", "inv", "bgr"})
        if (doc[k].is<int>()) {
          prefs.putUChar(k, doc[k].as<int>());
          reboot = true;
        }
      if (doc["rot"].is<int>()) {
        rotation = doc["rot"].as<int>() & 3;
        prefs.putUChar("rot", rotation);
        lcd.setRotation(rotation);
      }
      printInfo();
      if (reboot) {
        Serial.flush();
        ESP.restart();
      }
    }
    return;
  }

  char prevName[40] = "";
  if (nSess > 0) strlcpy(prevName, sess[focus].name, sizeof prevName);

  strlcpy(clockStr, doc["t"] | "", sizeof clockStr);
  nSess = 0;
  for (JsonObject o : doc["s"].as<JsonArray>()) {
    if (nSess >= MAX_SESS) break;
    Session& s = sess[nSess++];
    strlcpy(s.name, o["n"] | "", sizeof s.name);
    const char* st = o["st"] | "idle";
    s.state = !strcmp(st, "work") ? 'w' : !strcmp(st, "wait") ? 'q' : 'i';
    strlcpy(s.verb, o["v"] | "", sizeof s.verb);
    strlcpy(s.detail, o["d"] | "", sizeof s.detail);
    s.age = o["a"] | 0;
    s.ctx = o["c"] | -1;
    strlcpy(s.model, o["m"] | "", sizeof s.model);
  }

  auto readLimit = [&](const char* key, Limit& l) {
    JsonArray a = doc["l"][key];
    l.known = !a.isNull() && a.size() >= 2;
    if (l.known) {
      l.pct = a[0].as<float>();
      l.reset = a[1].as<int32_t>();
    }
  };
  readLimit("h5", lim5h);
  readLimit("d7", lim7d);

  // The bridge sorts by urgency, so session 0 is the one to show - unless
  // the user tapped to pick another one recently; then keep following it.
  focus = 0;
  if (manualFocusMs && millis() - manualFocusMs < 30000) {
    for (int i = 0; i < nSess; i++)
      if (!strcmp(sess[i].name, prevName)) focus = i;
  } else {
    manualFocusMs = 0;
  }
  rxMs = millis();
  everConnected = true;
}

static void pollSerial() {
  static char buf[4096];
  static size_t n = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      buf[n] = 0;
      if (n) parseLine(buf);
      n = 0;
    } else if (n < sizeof buf - 1) {
      buf[n++] = c;
    } else {
      n = 0;  // overlong line: drop it
    }
  }
}

// ------------------------------------------------------- touch / backlight --

static uint32_t lastWakeMs = 0;
static char lastSignature[64] = "";

static void wake() { lastWakeMs = millis(); }

static void pollTouch() {
  static uint32_t downMs = 0;
  static bool flipped = false;
  int32_t x, y;
  bool down = lcd.getTouch(&x, &y);
  if (down && !downMs) {
    downMs = millis();
    flipped = false;
  }
  if (down && !flipped && millis() - downMs > 1500) {  // long press: rotate 180
    rotation = rotation ^ 2;
    lcd.setRotation(rotation);
    prefs.putUChar("rot", rotation);
    flipped = true;
  }
  if (!down && downMs) {
    if (!flipped && millis() - downMs > 30) {
      bool wasDim = millis() - lastWakeMs > 10 * 60000;
      if (!wasDim && nSess > 1) {
        focus = (focus + 1) % nSess;
        manualFocusMs = millis();
      }
    }
    wake();
    downMs = 0;
  }
}

static void updateBacklightAndLed() {
  // Anything changing on screen counts as activity and keeps it bright.
  char sig[64];
  snprintf(sig, sizeof sig, "%d|%d|%c|%s", online(), nSess, nSess ? sess[0].state : '-',
           nSess ? sess[0].verb : "");
  bool busy = false;
  for (int i = 0; i < nSess; i++) busy |= sess[i].state != 'i';
  if (strcmp(sig, lastSignature) || (online() && busy)) {
    strlcpy(lastSignature, sig, sizeof lastSignature);
    wake();
  }
  uint32_t idle = millis() - lastWakeMs;
  uint8_t target = idle < 10 * 60000 ? 255 : (online() ? 40 : 6);
  static float level = 255;
  level += (target - level) * 0.08f;
  lcd.setBrightness((uint8_t)level);

  // Back-side RGB LED (active low): breathe amber while Claude waits on you.
  bool waiting = online() && nSess > 0 && sess[0].state == 'q';
  float b = waiting ? 0.5f + 0.5f * sinf(T * 4) : 0;
  ledcWrite(1, 255 - (uint8_t)(b * 255));
  ledcWrite(2, 255 - (uint8_t)(b * 70));
  ledcWrite(3, 255);
}

// ------------------------------------------------------------------ setup --

static uint32_t panelId = 0;

static void printInfo() {
  Serial.printf("PONG claudescreen 2 panel=%s id=%06lx inv=%d bgr=%d rot=%d\n",
                lcd.isST7789 ? "st7789" : "ili9341", (unsigned long)panelId, prefs.getUChar("inv", 0),
                prefs.getUChar("bgr", 0), rotation);
}

// Read the controller's ID (RDDID): ST7789 answers 85 85 52, ILI9341 mostly
// zeros. Then bring the display up with the right driver.
static void initDisplay() {
  uint8_t panel = prefs.getUChar("panel", 0), inv = prefs.getUChar("inv", 0);
  bool bgr = prefs.getUChar("bgr", 0);
  bool st = panel == 2;
  if (panel == 0) {
    lcd.select(false, false, bgr);
    lcd.init();
    panelId = lcd.getPanel()->readCommand(0x04, 1, 3) & 0xFFFFFF;
    st = (panelId & 0xFF) == 0x85 || ((panelId >> 16) & 0xFF) == 0x52;
  }
  bool invert = inv == 0 ? st : inv == 2;
  lcd.select(st, invert, bgr);
  lcd.init();
}

void setup() {
  Serial.setRxBufferSize(8192);
  Serial.begin(460800);

  const int pins[] = {PIN_LED_R, PIN_LED_G, PIN_LED_B};
  for (int i = 0; i < 3; i++) {
    ledcSetup(i + 1, 5000, 8);
    ledcAttachPin(pins[i], i + 1);
    ledcWrite(i + 1, 255);
  }

  prefs.begin("claudescreen", false);
  rotation = prefs.getUChar("rot", 1);

  initDisplay();
  lcd.initDMA();
  lcd.setRotation(rotation);
  lcd.setBrightness(255);
  lcd.fillScreen(C_BG);

  for (auto& b : band) {
    b.setColorDepth(16);
    b.setPsram(false);
    b.createSprite(W, BH);
  }
  loadFont(fHero, wHero, f_hero_vlw);
  loadFont(fTitle, wTitle, f_title_vlw);
  loadFont(fNum, wNum, f_num_vlw);
  loadFont(fBody, wBody, f_body_vlw);
  loadFont(fSmall, wSmall, f_small_vlw);
  loadFont(fMono, wMono, f_mono_vlw);

  wake();
  printInfo();
}

void loop() {
  pollSerial();
  pollTouch();
  T = millis() / 1000.0f;
  updateBacklightAndLed();

  // Animate at ~30 fps while something moves, otherwise just tick the timers.
  static uint32_t lastFrame = 0, lastRx = 0;
  bool animating = online() && nSess > 0 && sess[focus].state != 'i';
  animating |= online() && nSess > 0 && sess[0].state == 'q';
  uint32_t interval = animating ? 33 : 250;
  if (millis() - lastFrame >= interval || rxMs != lastRx || shotRequested) {
    lastFrame = millis();
    lastRx = rxMs;
    renderFrame();
  }
  delay(1);
}
