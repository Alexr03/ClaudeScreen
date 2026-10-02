// ClaudeScreen - Claude Code activity + plan usage on an ESP32-2432S028.
//
// PC bridges (bridge/claude_screen.py) send one JSON object per second, over
// USB serial and/or a WebSocket (see net.h):
//   {"t":"14:32","s":[{"n":"proj","st":"work","v":"Editing","d":"main.cpp",
//    "a":42,"c":31,"m":"Opus 5.5"}],"l":{"h5":[23.5,8040],"d7":[41,300000]}}
// st: work | wait | idle.  a: seconds in this state.  c: context % (-1 unknown).
// l.*: [used %, seconds until reset]; a missing window means "unknown".
//
// Each sender is a "source" (USB, or a paired PC). The screen merges the
// sessions of all live sources, most urgent first.
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

#ifndef FW_VERSION
#define FW_VERSION "dev"  // release builds pass the git tag
#endif
#define FW_VERSION_STR FW_VERSION

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
  uint8_t src;  // which source it came from
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
  int32_t reset = 0;  // seconds until reset, as of the source's rxMs
};

static constexpr int MAX_SESS = 6;   // per source
static constexpr int MAX_SRC = 5;    // 0 = USB, 1.. = PCs on the network
static constexpr int MAX_VIEW = 8;   // sessions on screen
static constexpr uint32_t STALE_MS = 6000;

struct Source {
  char label[32];  // "usb" or the PC's pairing label
  Session s[MAX_SESS];
  int n = 0;
  Limit l5, l7;
  char clock[8] = "";
  uint32_t rxMs = 0;  // 0 = never
};
static Source sources[MAX_SRC];

// The merged view the drawing code reads, rebuilt before every frame. Ages and
// reset timers in it are already brought up to date.
static Session sess[MAX_VIEW];
static int nSess = 0;
static Limit lim5h, lim7d;
static char clockStr[8] = "";
static uint32_t lastRxMs = 0;  // newest update from any source
static bool everConnected = false;
static bool multiSource = false;  // more than one PC live: label sessions

static int focus = 0;               // which session the hero card shows
static uint32_t manualFocusMs = 0;  // non-zero while the user picked one by tapping
static uint8_t focusSrc = 0;
static char focusName[40] = "";

// Network status, filled in by net.h and read by the drawing code.
struct NetUi {
  char id[8] = "";
  char hostname[32] = "";
  char name[40] = "";
  char ip[16] = "";
  bool wifi = false;
  bool portal = false;
  char apName[32] = "";
  char apPass[12] = "";
  bool pairing = false;
  char pairCode[8] = "";
  char pairLabel[32] = "";
  uint32_t pairUntil = 0;
  int otaPct = -1;
  char ssid[33] = "";
};
static NetUi netUi;
static int pairedCount();  // net.h

// Settings menu, opened with a long press.
enum Confirm { C_NONE, C_FORGET_WIFI, C_UNPAIR, C_RESET };
static bool menuOpen = false;
static Confirm menuConfirm = C_NONE;
static uint32_t menuTouchMs = 0;  // closes itself after a minute untouched
static uint8_t brightness = 100;  // percent
static bool statusLed = true;     // RGB LED on the back
static uint32_t bootHeldMs = 0;   // BOOT button held: factory reset countdown
static constexpr int MENU_ROW_Y = 42, MENU_ROW_H = 34, MENU_ROW_GAP = 4, MENU_ROWS = 8;
enum MenuRow { R_WIFI, R_PAIRED, R_BRIGHT, R_LED, R_ROTATE, R_CALIBRATE, R_FIRMWARE, R_RESET };
static int menuScroll = 0;  // px; drag the list to scroll

static int menuMaxScroll() {
  return max(0, MENU_ROW_Y + MENU_ROWS * (MENU_ROW_H + MENU_ROW_GAP) + 2 - 240);  // 240 = screen height
}

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
static bool online() { return everConnected && millis() - lastRxMs < STALE_MS; }

// Three arcs and a dot, bottom-centred on (x, y).
static void wifiGlyph(float x, float y, uint16_t col) {
  for (int i = 0; i < 3; i++) {
    float r = 3.5f + i * 3.6f;
    paintSDF(x - r - 2, y - r - 2, x + r + 2, y + 1, col, 1, [&](float px, float py) {
      float dx = px - x, dy = py - y;
      if (fabsf(atan2f(dx, -dy)) > 0.8f) return 9.0f;
      return fabsf(sqrtf(dx * dx + dy * dy) - r) - 0.9f;
    });
  }
  disc(x, y - 0.5f, 1.4f, col);
}

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
  int cr = 312;
  if (online()) {
    text(fTitle, clockStr, cr, 23, C_MUTED, textdatum_t::baseline_right);
    cr -= textW(fTitle, clockStr) + 10;
  }
  if (netUi.wifi) wifiGlyph(cr - 7, 22, C_DIM);
}

static void drawHero() {
  const int cx = 8, cy = 34, cw = 304, ch = 100;
  rrect(cx, cy, cw, ch, 12, C_CARD);
  const float ox = 50, oy = cy + ch / 2;
  const int tx = 94, tr = cx + cw - 12, tw = tr - tx;
  char buf[112], line[112];

  if (netUi.pairing) {
    uint32_t left = netUi.pairUntil > millis() ? (netUi.pairUntil - millis()) / 1000 : 0;
    glow(ox, oy, 44, C_AMBER, 0.14f + 0.08f * sinf(T * 3));
    spark(ox, oy, 25, C_AMBER, T, 0, 0);
    text(fSmall, "PAIRING REQUEST", tx, 54, C_AMBER);
    snprintf(buf, sizeof buf, "from %s", netUi.pairLabel);
    text(fSmall, fit(fSmall, buf, tw - 110, line, sizeof line), tr, 54, C_MUTED, textdatum_t::baseline_right);
    snprintf(buf, sizeof buf, "%.3s %.3s", netUi.pairCode, netUi.pairCode + 3);
    text(fHero, buf, tx, 88, C_TEXT);
    snprintf(buf, sizeof buf, "enter this code on that PC  \xC2\xB7  %us", (unsigned)left);
    text(fSmall, buf, tx, 112, C_DIM);
    return;
  }
  if (!online()) {
    spark(ox, oy, 24, C_DIM, T, 0, 0);
    if (netUi.portal) {
      text(fSmall, "WIFI SETUP", tx, 54, C_MUTED);
      text(fHero, "Set up WiFi", tx, 85, C_TEXT);
      snprintf(buf, sizeof buf, "join %s", netUi.apName);
      text(fMono, fit(fMono, buf, tw, line, sizeof line), tx, 104, C_TEXT);
      snprintf(buf, sizeof buf, "password %s", netUi.apPass);
      text(fMono, buf, tx, 122, C_MUTED);
      return;
    }
    text(fSmall, everConnected ? "CONNECTION LOST" : "CLAUDESCREEN", tx, 54, C_DIM);
    text(fHero, "Waiting for PC", tx, 85, C_MUTED);
    if (netUi.wifi) {
      snprintf(buf, sizeof buf, "%s.local", netUi.hostname);
      text(fMono, fit(fMono, buf, tw, line, sizeof line), tx, 104, C_DIM);
      text(fSmall, pairedCount() ? netUi.ip : "pair: claude_screen.py pair", tx, 122, C_DIM);
    } else {
      text(fMono, "connect USB or WiFi", tx, 104, C_DIM);
    }
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
  fmtDur(s.age, age, sizeof age, true);
  int ageW = textW(fMono, age);
  text(fMono, age, tr, 54, C_DIM, textdatum_t::baseline_right);
  int n = snprintf(buf, sizeof buf, "%s", s.name);
  if (multiSource) n += snprintf(buf + n, sizeof buf - n, "  \xC2\xB7  %s", sources[s.src].label);
  if (nSess > 1) snprintf(buf + n, sizeof buf - n, "  \xC2\xB7  %d of %d", focus + 1, nSess);
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
  int32_t left = l.known ? l.reset : 0;
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
    strlcpy(rs, "no data yet", sizeof rs);
  }
  text(fTitle, ringLabel, rx, ry + 6, C_TEXT, textdatum_t::baseline_center);
  const int tx = x + 68;
  text(fSmall, label, tx, y + 20, C_MUTED);
  text(fHero, pct, tx - 1, y + 48, l.known ? (p >= 75 ? usageColor(p) : C_TEXT) : C_DIM);
  text(fSmall, rs, tx, y + 63, C_DIM);
}

static void button(int x, int y, int w, int h, const char* label, uint16_t bg, uint16_t fg) {
  rrect(x, y, w, h, 10, bg);
  text(fBody, label, x + w / 2, y + h / 2 + 5, fg, textdatum_t::baseline_center);
}

static void drawMenu() {
  char buf[64];
  const char* labels[MENU_ROWS] = {"WiFi",           "Paired PCs", "Brightness", "Status light", "Rotate screen",
                                   "Calibrate touch", "Firmware",   "Factory reset"};
  for (int i = 0; i < MENU_ROWS; i++) {
    int y = MENU_ROW_Y + i * (MENU_ROW_H + MENU_ROW_GAP) - menuScroll;
    if (y + MENU_ROW_H < MENU_ROW_Y - 4 || y > H) continue;
    rrect(8, y, 304, MENU_ROW_H, 9, C_CARD);
    text(fBody, labels[i], 20, y + 22, i == R_RESET ? C_RED : C_TEXT);
    buf[0] = 0;
    if (i == 0) {
      if (netUi.wifi) snprintf(buf, sizeof buf, "%s  \xC2\xB7  %s", netUi.ssid, netUi.ip);
      else strlcpy(buf, netUi.portal ? "setup network on" : "off  \xC2\xB7  tap to set up", sizeof buf);
    }
    if (i == 1) snprintf(buf, sizeof buf, "%d  \xC2\xB7  tap to forget all", pairedCount());
    if (i == 2) snprintf(buf, sizeof buf, "%d%%", brightness);
    if (i == R_LED) strlcpy(buf, statusLed ? "on  \xC2\xB7  pulses when Claude needs you" : "off", sizeof buf);
    if (i == R_FIRMWARE) snprintf(buf, sizeof buf, "%s  \xC2\xB7  %s", FW_VERSION_STR, netUi.hostname);
    char line[64];
    if (*buf) text(fSmall, fit(fSmall, buf, 190, line, sizeof line), 300, y + 22, C_MUTED, textdatum_t::baseline_right);
  }

  // Scroll indicator, when the rows don't all fit.
  int maxScroll = menuMaxScroll();
  if (maxScroll > 0) {
    float view = H - MENU_ROW_Y, total = view + maxScroll;
    float th = view * view / total, ty = MENU_ROW_Y + (view - th) * menuScroll / maxScroll;
    capsule(316, ty + 3, 316, ty + th - 3, 1.5f, C_TRACK);
  }

  // Header on top, so rows scroll underneath it.
  if (OY < MENU_ROW_Y - 4) S->fillRect(0, -OY, W, MENU_ROW_Y - 4, C_BG);
  text(fTitle, "Settings", 16, 25, C_TEXT);
  strlcpy(buf, netUi.name, sizeof buf);
  text(fSmall, buf, 16 + textW(fTitle, "Settings") + 10, 24, C_DIM);
  disc(294, 18, 13, C_CARD);  // close button
  capsule(289, 13, 299, 23, 1.2f, C_MUTED);
  capsule(299, 13, 289, 23, 1.2f, C_MUTED);

  if (menuConfirm != C_NONE) {
    const char* title = menuConfirm == C_FORGET_WIFI ? "Forget WiFi?"
                      : menuConfirm == C_UNPAIR      ? "Forget all paired PCs?"
                                                     : "Factory reset?";
    const char* sub = menuConfirm == C_FORGET_WIFI ? "The screen will start its setup network."
                    : menuConfirm == C_UNPAIR      ? "Each PC will need to pair again."
                                                   : "Forgets WiFi, paired PCs and settings.";
    rrect(24, 62, 272, 120, 14, C_TRACK);
    text(fTitle, title, 160, 92, C_TEXT, textdatum_t::baseline_center);
    text(fSmall, sub, 160, 114, C_MUTED, textdatum_t::baseline_center);
    button(40, 130, 114, 36, "Cancel", C_CARD, C_TEXT);
    button(166, 130, 114, 36, menuConfirm == C_RESET ? "Reset" : "Forget", C_RED, C_TEXT);
  }
}

static void drawResetCountdown() {
  int left = 5 - (int)((millis() - bootHeldMs) / 1000);
  rrect(24, 70, 272, 100, 14, C_TRACK);
  text(fTitle, "Factory reset", 160, 102, C_RED, textdatum_t::baseline_center);
  char buf[48];
  snprintf(buf, sizeof buf, "keep holding BOOT  \xC2\xB7  %ds", max(left, 0));
  text(fSmall, buf, 160, 126, C_MUTED, textdatum_t::baseline_center);
  text(fSmall, "release to cancel", 160, 146, C_DIM, textdatum_t::baseline_center);
}

static void drawScene() {
  S->fillSprite(C_BG);
  if (menuOpen) {
    drawMenu();
    if (bootHeldMs) drawResetCountdown();
    return;
  }
  drawHeader();
  drawHero();
  drawChips();
  drawTile(8, "5h", "Session", lim5h);
  drawTile(164, "7d", "Weekly", lim7d);
  if (bootHeldMs) drawResetCountdown();
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

// ---------------------------------------------------------------- sources --

static void wake();

// The source slot for a PC label, reusing its old slot after a reconnect.
static int sourceFor(const char* label) {
  int oldest = 1;
  for (int i = 1; i < MAX_SRC; i++) {
    if (!strcmp(sources[i].label, label)) return i;
    if (sources[i].rxMs < sources[oldest].rxMs) oldest = i;
  }
  for (int i = 1; i < MAX_SRC; i++)
    if (!sources[i].label[0]) oldest = i;
  sources[oldest] = Source();
  strlcpy(sources[oldest].label, label, sizeof sources[oldest].label);
  return oldest;
}

static bool usbRx = false;
static bool usbSeen() { return usbRx; }

static void applyState(JsonDocument& doc, int idx, const char* label) {
  Source& src = sources[idx];
  strlcpy(src.label, label, sizeof src.label);
  strlcpy(src.clock, doc["t"] | "", sizeof src.clock);
  src.n = 0;
  for (JsonObject o : doc["s"].as<JsonArray>()) {
    if (src.n >= MAX_SESS) break;
    Session& s = src.s[src.n++];
    s.src = idx;
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
  readLimit("h5", src.l5);
  readLimit("d7", src.l7);
  src.rxMs = millis();
  lastRxMs = src.rxMs;
  everConnected = true;
}

// Merge every live source into the view: waiting sessions first, then
// working, then idle, each in the order their bridge sent them.
static void rebuildView() {
  uint32_t now = millis();
  int live = 0, newest = -1;
  for (int i = 0; i < MAX_SRC; i++) {
    const Source& s = sources[i];
    if (!s.rxMs || now - s.rxMs >= STALE_MS) continue;
    live++;
    if (newest < 0 || s.rxMs > sources[newest].rxMs) newest = i;
  }
  multiSource = live > 1;
  strlcpy(clockStr, newest >= 0 ? sources[newest].clock : "", sizeof clockStr);

  nSess = 0;
  for (char prio : {'q', 'w', 'i'})
    for (int i = 0; i < MAX_SRC; i++) {
      const Source& src = sources[i];
      if (!src.rxMs || now - src.rxMs >= STALE_MS) continue;
      for (int k = 0; k < src.n && nSess < MAX_VIEW; k++) {
        if (src.s[k].state != prio) continue;
        sess[nSess] = src.s[k];
        sess[nSess].age += (now - src.rxMs) / 1000;
        nSess++;
      }
    }

  // The most urgent session gets the hero card - unless the user tapped to
  // pick one in the last 30s; then keep following that one.
  focus = 0;
  if (manualFocusMs && now - manualFocusMs < 30000) {
    for (int i = 0; i < nSess; i++)
      if (sess[i].src == focusSrc && !strcmp(sess[i].name, focusName)) focus = i;
  } else {
    manualFocusMs = 0;
  }

  // Usage belongs to an account, so show the focused session's PC's numbers.
  int limSrc = nSess ? sess[focus].src : newest;
  if (limSrc >= 0 && !sources[limSrc].l5.known && !sources[limSrc].l7.known) limSrc = newest;
  lim5h = lim7d = Limit();
  if (limSrc >= 0) {
    int32_t elapsed = (now - sources[limSrc].rxMs) / 1000;
    lim5h = sources[limSrc].l5;
    lim7d = sources[limSrc].l7;
    lim5h.reset -= elapsed;
    lim7d.reset -= elapsed;
  }
}

// ------------------------------------------------------- touch / backlight --

static uint32_t lastWakeMs = 0;
static char lastSignature[64] = "";

static void wake() { lastWakeMs = millis(); }

static void openMenu();
static void menuTap(int x, int y);

static void pollTouch() {
  static uint32_t downMs = 0;
  static bool held = false;
  static int32_t tx = 0, ty = 0, startY = 0, startScroll = 0;
  static bool dragging = false;
  int32_t x, y;
  bool down = lcd.getTouch(&x, &y);
  if (down) {
    tx = x;
    ty = y;
  }
  if (down && !downMs) {
    downMs = millis();
    held = false;
    dragging = false;
    startY = y;
    startScroll = menuScroll;
  }
  // In the menu, a vertical drag scrolls the list instead of tapping.
  if (down && menuOpen && menuConfirm == C_NONE && (dragging || abs(y - startY) > 8)) {
    dragging = true;
    held = true;  // so releasing doesn't count as a tap
    menuScroll = constrain(startScroll - (y - startY), 0, menuMaxScroll());
    menuTouchMs = millis();
  }
  if (down && !held && !menuOpen && millis() - downMs > 1000) {  // long press: settings
    held = true;
    openMenu();
  }
  if (!down && downMs) {
    bool wasDim = millis() - lastWakeMs > 10 * 60000;
    if (!held && millis() - downMs > 30 && !wasDim) {
      if (menuOpen) {
        menuTap(tx, ty);
      } else if (nSess > 1) {
        const Session& next = sess[(focus + 1) % nSess];
        focusSrc = next.src;
        strlcpy(focusName, next.name, sizeof focusName);
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
  snprintf(sig, sizeof sig, "%d|%d|%c|%s|%d|%d", online(), nSess, nSess ? sess[0].state : '-',
           nSess ? sess[0].verb : "", netUi.pairing, netUi.portal);
  bool busy = false;
  for (int i = 0; i < nSess; i++) busy |= sess[i].state != 'i';
  if (strcmp(sig, lastSignature) || (online() && busy)) {
    strlcpy(lastSignature, sig, sizeof lastSignature);
    wake();
  }
  uint32_t idle = millis() - lastWakeMs;
  uint8_t full = brightness * 255 / 100;
  uint8_t target = idle < 10 * 60000 || menuOpen ? full : (online() ? 40 : 6);
  static float level = 255;
  level += (target - level) * 0.08f;
  lcd.setBrightness((uint8_t)level);

  // Back-side RGB LED (active low): breathe amber while Claude waits on you.
  bool waiting = statusLed && online() && nSess > 0 && sess[0].state == 'q';
  float b = waiting ? 0.5f + 0.5f * sinf(T * 4) : 0;
  ledcWrite(1, 255 - (uint8_t)(b * 255));
  ledcWrite(2, 255 - (uint8_t)(b * 70));
  ledcWrite(3, 255);
}

// Drawn straight to the panel: the main loop is blocked during an update.
static void drawOtaProgress() {
  lcd.setBrightness(255);
  lcd.startWrite();
  lcd.fillRect(0, 90, W, 70, C_BG);
  lcd.setFont(&fTitle);
  lcd.setTextColor(C_TEXT, C_BG);
  lcd.setTextDatum(textdatum_t::baseline_center);
  char buf[32];
  snprintf(buf, sizeof buf, "Updating firmware  %d%%", netUi.otaPct);
  lcd.drawString(buf, W / 2, 112);
  lcd.fillRoundRect(40, 128, W - 80, 8, 4, C_TRACK);
  lcd.fillRoundRect(40, 128, (W - 80) * netUi.otaPct / 100, 8, 4, C_CORAL);
  lcd.endWrite();
}

// ------------------------------------------------------------------ setup --

static uint32_t panelId = 0;

#include "net.h"

// ------------------------------------------------------------------ menu --

static void loadTouchCalibration() {
  uint16_t cal[8];
  if (prefs.getBytes("tcal", cal, sizeof cal) == sizeof cal) lcd.setTouchCalibrate(cal);
}

// Tap the four corners: makes taps land where they should on this panel.
static void calibrateTouch() {
  lcd.setBrightness(255);
  lcd.fillScreen(C_BG);
  lcd.setFont(&fTitle);
  lcd.setTextColor(C_TEXT, C_BG);
  lcd.setTextDatum(textdatum_t::middle_center);
  lcd.drawString("Touch calibration", W / 2, H / 2 - 12);
  lcd.setFont(&fSmall);
  lcd.setTextColor(C_MUTED, C_BG);
  lcd.drawString("tap each corner marker as it appears", W / 2, H / 2 + 12);
  delay(1500);
  uint16_t cal[8];
  lcd.calibrateTouch(cal, C_CORAL, C_BG, 14);
  prefs.putBytes("tcal", cal, sizeof cal);
  lcd.setTouchCalibrate(cal);
  lcd.fillScreen(C_BG);
}

static void openMenu() {
  if (!prefs.isKey("tcal")) calibrateTouch();
  menuOpen = true;
  menuConfirm = C_NONE;
  menuScroll = 0;
  menuTouchMs = millis();
}

// Keeps the panel settings (wrong ones can leave the screen unreadable) and
// the touch calibration; everything else goes.
static void factoryReset() {
  uint8_t panel = prefs.getUChar("panel", 0), inv = prefs.getUChar("inv", 0), bgr = prefs.getUChar("bgr", 0);
  uint16_t cal[8];
  bool hasCal = prefs.getBytes("tcal", cal, sizeof cal) == sizeof cal;
  prefs.clear();
  prefs.putUChar("panel", panel);
  prefs.putUChar("inv", inv);
  prefs.putUChar("bgr", bgr);
  if (hasCal) prefs.putBytes("tcal", cal, sizeof cal);
  wm.resetSettings();
  WiFi.disconnect(true, true);
  lcd.fillScreen(C_BG);
  delay(200);
  ESP.restart();
}

static void menuTap(int x, int y) {
  menuTouchMs = millis();
  if (menuConfirm != C_NONE) {
    if (y >= 120 && y <= 176) {
      if (x >= 160) {
        if (menuConfirm == C_FORGET_WIFI) {
          forgetWifi();
          startPortal();
        } else if (menuConfirm == C_UNPAIR) {
          memset(paired, 0, sizeof paired);
          savePaired();
          for (int i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++) ws.disconnect(i);
        } else if (menuConfirm == C_RESET) {
          factoryReset();
        }
      }
      menuConfirm = C_NONE;
    }
    return;
  }
  if (x > 260 && y < 38) {  // close
    menuOpen = false;
    return;
  }
  if (y < MENU_ROW_Y - 4) return;  // header
  int pos = y - MENU_ROW_Y + menuScroll;
  int row = pos / (MENU_ROW_H + MENU_ROW_GAP);
  if (pos < 0 || row >= MENU_ROWS || pos % (MENU_ROW_H + MENU_ROW_GAP) > MENU_ROW_H) return;
  switch (row) {
    case R_WIFI:
      if (netUi.wifi) menuConfirm = C_FORGET_WIFI;
      else if (!netUi.portal) startPortal();
      break;
    case R_PAIRED:
      if (pairedCount()) menuConfirm = C_UNPAIR;
      break;
    case R_BRIGHT:
      brightness = brightness >= 100 ? 40 : brightness + 30;
      prefs.putUChar("bright", brightness);
      break;
    case R_LED:
      statusLed = !statusLed;
      prefs.putBool("led", statusLed);
      break;
    case R_ROTATE:
      rotation ^= 2;
      lcd.setRotation(rotation);
      prefs.putUChar("rot", rotation);
      break;
    case R_CALIBRATE:
      calibrateTouch();
      menuTouchMs = millis();
      break;
    case R_RESET: menuConfirm = C_RESET; break;
  }
}

// BOOT (GPIO0) held for 5 seconds: factory reset, for when touch won't do.
static void pollBootButton() {
  bool down = digitalRead(0) == LOW;
  if (down && !bootHeldMs) bootHeldMs = millis();
  if (!down) bootHeldMs = 0;
  if (bootHeldMs && millis() - bootHeldMs > 5000) factoryReset();
  if (menuOpen && millis() - menuTouchMs > 60000) menuOpen = false;
}

static void printInfo() {
  Serial.printf("PONG claudescreen 2 fw=%s panel=%s id=%06lx inv=%d bgr=%d rot=%d host=%s name=\"%s\" wifi=%s ip=%s paired=%d\n",
                FW_VERSION, lcd.isST7789 ? "st7789" : "ili9341", (unsigned long)panelId, prefs.getUChar("inv", 0),
                prefs.getUChar("bgr", 0), rotation, netUi.hostname, netUi.name,
                netUi.wifi ? WiFi.SSID().c_str() : (netUi.portal ? "setup" : "off"), netUi.wifi ? netUi.ip : "-",
                pairedCount());
}

// ----------------------------------------------------------------- serial --

static void parseLine(char* line) {
  JsonDocument doc;
  if (deserializeJson(doc, line)) return;

  if (!doc["cmd"].is<const char*>()) {
    // A bridge names itself ("o"), so a PC connected over both USB and WiFi
    // fills one source instead of showing every session twice.
    usbRx = true;
    const char* origin = doc["o"] | "";
    if (*origin) applyState(doc, sourceFor(origin), origin);
    else applyState(doc, 0, "usb");
    return;
  }
  const char* cmd = doc["cmd"];
  if (!strcmp(cmd, "shot")) shotRequested = true;
  if (!strcmp(cmd, "ping")) printInfo();
  if (!strcmp(cmd, "unpair")) {  // forget every paired PC
    memset(paired, 0, sizeof paired);
    savePaired();
    for (int i = 0; i < WEBSOCKETS_SERVER_CLIENT_MAX; i++) ws.disconnect(i);
    printInfo();
  }
  // Settings, stored on the board:
  //   panel 0 auto|1 ILI9341|2 ST7789, inv 0 auto|1 off|2 on, bgr 0|1 (these reboot)
  //   rot 0-3, name "...", wifi_ssid + wifi_pass, forget_wifi 1
  if (!strcmp(cmd, "set")) {
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
    if (doc["name"].is<const char*>()) {
      prefs.putString("name", (const char*)doc["name"]);
      reboot = true;  // re-announce on mDNS under the new name
    }
    if (doc["wifi_ssid"].is<const char*>()) setWifi(doc["wifi_ssid"], doc["wifi_pass"] | "");
    if (doc["forget_wifi"] | 0) forgetWifi();
    printInfo();
    if (reboot) {
      Serial.flush();
      ESP.restart();
    }
  }
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
  strlcpy(sources[0].label, "usb", sizeof sources[0].label);

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

  pinMode(0, INPUT_PULLUP);
  brightness = prefs.getUChar("bright", 100);
  statusLed = prefs.getBool("led", true);
  loadTouchCalibration();
  netSetup();
  wake();
  printInfo();
}

void loop() {
  pollSerial();
  netLoop();
  pollTouch();
  pollBootButton();
  T = millis() / 1000.0f;
  rebuildView();
  updateBacklightAndLed();

  // Animate at ~30 fps while something moves, otherwise just tick the timers.
  static uint32_t lastFrame = 0, lastRx = 0;
  bool animating = online() && nSess > 0 && sess[focus].state != 'i';
  animating |= online() && nSess > 0 && sess[0].state == 'q';
  animating |= netUi.pairing || bootHeldMs || menuOpen;  // menu: smooth scrolling
  uint32_t interval = animating ? 33 : 250;
  if (millis() - lastFrame >= interval || lastRxMs != lastRx || shotRequested) {
    lastFrame = millis();
    lastRx = lastRxMs;
    renderFrame();
  }
  delay(1);
}
