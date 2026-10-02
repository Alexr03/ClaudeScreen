// WiFi transport: mDNS discovery, a WebSocket server for bridges, pairing,
// and over-the-air updates for paired PCs. Included once, from main.cpp.
//
// Every screen announces itself as claudescreen-<id>.local with a
// _claudescreen._tcp service. A bridge connects to ws://<screen>:81 and gets
//   {"hello":"claudescreen","id":"a1b2","name":"...","nonce":"<32 hex>"}
// then either authenticates with a key from an earlier pairing:
//   {"auth":"<label>","mac":hex(HMAC-SHA256(key, nonce))}
// or pairs: {"pair":"<label>"} makes the screen show a 6-digit code, and
//   {"pair_proof":hex(HMAC-SHA256(code, nonce || label))}
// proves the user read it. The screen answers with a fresh random key,
// XOR-masked with SHA256(code || nonce || "claudescreen-key"). Only
// authenticated connections may update the display.
//
// The mask stops a casual sniffer, but six digits can be brute-forced
// offline from a captured exchange: pairing protects against other bridges
// on the network, not against someone recording your WiFi at that moment.
#pragma once

#include <ArduinoOTA.h>
#include <ESPmDNS.h>
#include <WebSocketsServer.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_random.h>
#include <mbedtls/md.h>

static constexpr int WS_PORT = 81;
static constexpr int MAX_PAIRED = 8;

struct PairedClient {
  char label[32];
  uint8_t key[32];
};

struct Conn {
  bool open = false;
  bool authed = false;
  int src = -1;
  uint8_t nonce[16];
  char label[32];
  int pairTries = 0;
};

static WebSocketsServer ws(WS_PORT);
static WiFiManager wm;
static Conn conns[WEBSOCKETS_SERVER_CLIENT_MAX];
static PairedClient paired[MAX_PAIRED];
static int pairingConn = -1;
static bool mdnsUp = false, otaUp = false;
static uint32_t otaUntil = 0;
static uint32_t bootMs = 0;

// ---------------------------------------------------------------- helpers --

static void toHex(const uint8_t* b, size_t n, char* out) {
  static const char* d = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[2 * i] = d[b[i] >> 4];
    out[2 * i + 1] = d[b[i] & 15];
  }
  out[2 * n] = 0;
}

static bool fromHex(const char* s, uint8_t* out, size_t n) {
  if (!s || strlen(s) != 2 * n) return false;
  for (size_t i = 0; i < n; i++) {
    char hi = tolower(s[2 * i]), lo = tolower(s[2 * i + 1]);
    if (!isxdigit(hi) || !isxdigit(lo)) return false;
    out[i] = ((isdigit(hi) ? hi - '0' : hi - 'a' + 10) << 4) | (isdigit(lo) ? lo - '0' : lo - 'a' + 10);
  }
  return true;
}

static void hmac(const uint8_t* key, size_t klen, const uint8_t* msg, size_t mlen, uint8_t out[32]) {
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, klen, msg, mlen, out);
}

static void sha256(const uint8_t* msg, size_t len, uint8_t out[32]) {
  mbedtls_md(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), msg, len, out);
}

// Constant-time compare, so response timing doesn't leak how close a guess was.
static bool sameBytes(const uint8_t* a, const uint8_t* b, size_t n) {
  uint8_t d = 0;
  for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
  return d == 0;
}

static void loadPaired() {
  memset(paired, 0, sizeof paired);
  prefs.getBytes("paired", paired, sizeof paired);
}

static void savePaired() { prefs.putBytes("paired", paired, sizeof paired); }

static int pairedCount() {
  int n = 0;
  for (auto& p : paired) n += p.label[0] != 0;
  return n;
}

static void sendJson(uint8_t num, JsonDocument& doc) {
  String out;
  serializeJson(doc, out);
  ws.sendTXT(num, out);
}

static void sendErr(uint8_t num, const char* msg) {
  JsonDocument d;
  d["err"] = msg;
  sendJson(num, d);
}

static void endPairing() {
  pairingConn = -1;
  netUi.pairing = false;
}

// -------------------------------------------------------------------- OTA --

// A paired PC asks for an update window with a one-time password; the
// standard ArduinoOTA upload then has 10 minutes to use it. Nothing is stored,
// so only PCs that can authenticate can ever flash the screen.
static void otaStart(const char* pw) {
  if (otaUp) ArduinoOTA.end();
  ArduinoOTA.setHostname(netUi.hostname);
  ArduinoOTA.setMdnsEnabled(false);  // already announced by netLoop
  ArduinoOTA.setPassword(pw);
  ArduinoOTA.onStart([] {
    netUi.otaPct = 0;
    drawOtaProgress();
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    int pct = total ? done * 100 / total : 0;
    if (pct != netUi.otaPct) {
      netUi.otaPct = pct;
      drawOtaProgress();
    }
  });
  ArduinoOTA.onError([](ota_error_t) { netUi.otaPct = -1; });
  ArduinoOTA.begin();
  otaUp = true;
  otaUntil = millis() + 600000;
}

// ------------------------------------------------------------- websocket --

static void onHello(uint8_t num) {
  Conn& c = conns[num];
  c = Conn();
  c.open = true;
  esp_fill_random(c.nonce, sizeof c.nonce);
  char hex[33];
  toHex(c.nonce, sizeof c.nonce, hex);
  JsonDocument d;
  d["hello"] = "claudescreen";
  d["proto"] = 2;
  d["id"] = netUi.id;
  d["name"] = netUi.name;
  d["fw"] = FW_VERSION;
  d["nonce"] = hex;
  sendJson(num, d);
}

static void onPairRequest(uint8_t num, const char* label) {
  if (pairingConn >= 0 && pairingConn != num && millis() < netUi.pairUntil) {
    sendErr(num, "screen is already pairing with another PC");
    return;
  }
  Conn& c = conns[num];
  strlcpy(c.label, label, sizeof c.label);
  c.pairTries = 0;
  pairingConn = num;
  snprintf(netUi.pairCode, sizeof netUi.pairCode, "%06lu", (unsigned long)(esp_random() % 1000000));
  strlcpy(netUi.pairLabel, label, sizeof netUi.pairLabel);
  netUi.pairUntil = millis() + 120000;
  netUi.pairing = true;
  wake();
  JsonDocument d;
  d["pairing"] = 1;
  sendJson(num, d);
}

static void onPairProof(uint8_t num, const char* proofHex) {
  Conn& c = conns[num];
  if (pairingConn != num || !netUi.pairing || millis() > netUi.pairUntil) {
    sendErr(num, "no pairing in progress");
    return;
  }
  uint8_t msg[16 + sizeof c.label], expect[32], got[32];
  size_t llen = strlen(c.label);
  memcpy(msg, c.nonce, 16);
  memcpy(msg + 16, c.label, llen);
  hmac((const uint8_t*)netUi.pairCode, 6, msg, 16 + llen, expect);
  if (!fromHex(proofHex, got, 32) || !sameBytes(got, expect, 32)) {
    if (++c.pairTries >= 3) {
      endPairing();
      sendErr(num, "wrong code, pairing cancelled");
    } else {
      sendErr(num, "wrong code");
    }
    return;
  }

  // Store (or replace) this PC's key.
  int slot = -1;
  for (int i = 0; i < MAX_PAIRED; i++)
    if (!strcmp(paired[i].label, c.label)) slot = i;
  for (int i = 0; i < MAX_PAIRED && slot < 0; i++)
    if (!paired[i].label[0]) slot = i;
  if (slot < 0) slot = 0;  // full: replace the oldest entry
  strlcpy(paired[slot].label, c.label, sizeof paired[slot].label);
  esp_fill_random(paired[slot].key, 32);
  savePaired();

  uint8_t maskIn[6 + 16 + 16], mask[32], masked[32];
  memcpy(maskIn, netUi.pairCode, 6);
  memcpy(maskIn + 6, c.nonce, 16);
  memcpy(maskIn + 22, "claudescreen-key", 16);
  sha256(maskIn, sizeof maskIn, mask);
  for (int i = 0; i < 32; i++) masked[i] = paired[slot].key[i] ^ mask[i];
  char hex[65];
  toHex(masked, 32, hex);

  c.authed = true;
  c.src = sourceFor(c.label);
  endPairing();
  JsonDocument d;
  d["paired"] = hex;
  d["id"] = netUi.id;
  d["name"] = netUi.name;
  sendJson(num, d);
}

static void onAuth(uint8_t num, const char* label, const char* macHex) {
  Conn& c = conns[num];
  uint8_t got[32], expect[32];
  for (auto& p : paired) {
    if (!p.label[0] || strcmp(p.label, label)) continue;
    hmac(p.key, 32, c.nonce, 16, expect);
    if (fromHex(macHex, got, 32) && sameBytes(got, expect, 32)) {
      strlcpy(c.label, label, sizeof c.label);
      c.authed = true;
      c.src = sourceFor(label);
      JsonDocument d;
      d["ok"] = 1;
      d["name"] = netUi.name;
      sendJson(num, d);
      return;
    }
  }
  sendErr(num, "not paired with this screen");
  ws.disconnect(num);
}

static void onText(uint8_t num, uint8_t* payload, size_t len) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, len)) return;
  Conn& c = conns[num];

  if (doc["pair"].is<const char*>()) return onPairRequest(num, doc["pair"]);
  if (doc["pair_proof"].is<const char*>()) return onPairProof(num, doc["pair_proof"]);
  if (doc["auth"].is<const char*>()) return onAuth(num, doc["auth"], doc["mac"] | "");
  if (!c.authed) return sendErr(num, "not authenticated");

  if (doc["cmd"].is<const char*>()) {
    if (!strcmp(doc["cmd"], "ota") && doc["pw"].is<const char*>() && strlen(doc["pw"]) >= 16) {
      otaStart(doc["pw"]);
      JsonDocument d;
      d["ota"] = 3232;
      sendJson(num, d);
    }
    if (!strcmp(doc["cmd"], "unpair")) {  // a PC forgetting this screen
      for (auto& p : paired)
        if (!strcmp(p.label, c.label)) memset(&p, 0, sizeof p);
      savePaired();
      ws.disconnect(num);
    }
    return;
  }
  if (c.src >= 0) applyState(doc, c.src, c.label);
}

static void onWsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t len) {
  switch (type) {
    case WStype_CONNECTED: onHello(num); break;
    case WStype_TEXT: onText(num, payload, len); break;
    case WStype_DISCONNECTED:
      conns[num] = Conn();
      if (pairingConn == num) endPairing();
      break;
    default: break;
  }
}

// ------------------------------------------------------------------ setup --

static void startPortal() {
  if (netUi.portal) return;
  // A random password, shown on screen, so housemates can't reconfigure it.
  static const char* alnum = "abcdefghjkmnpqrstuvwxyz23456789";
  for (int i = 0; i < 8; i++) netUi.apPass[i] = alnum[esp_random() % strlen(alnum)];
  netUi.apPass[8] = 0;
  snprintf(netUi.apName, sizeof netUi.apName, "ClaudeScreen-%s", netUi.id);
  wm.setConfigPortalBlocking(false);
  wm.setConfigPortalTimeout(0);
  wm.setTitle("ClaudeScreen");
  wm.startConfigPortal(netUi.apName, netUi.apPass);
  netUi.portal = true;
  wake();
}

static void setWifi(const char* ssid, const char* pass) {
  if (netUi.portal) {
    wm.stopConfigPortal();
    netUi.portal = false;
  }
  WiFi.persistent(true);
  WiFi.begin(ssid, pass);
}

static void forgetWifi() {
  wm.resetSettings();
  WiFi.disconnect(true, true);
  netUi.wifi = false;
  bootMs = millis();  // the setup network follows after the usual grace period
}

static void netSetup() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(netUi.id, sizeof netUi.id, "%02x%02x", mac[4], mac[5]);
  snprintf(netUi.hostname, sizeof netUi.hostname, "claudescreen-%s", netUi.id);
  String name = prefs.getString("name", "");
  if (name.length()) strlcpy(netUi.name, name.c_str(), sizeof netUi.name);
  else snprintf(netUi.name, sizeof netUi.name, "ClaudeScreen %s", netUi.id);
  loadPaired();

  WiFi.mode(WIFI_STA);
  WiFi.setHostname(netUi.hostname);
  WiFi.setSleep(false);  // modem sleep adds ~100ms+ latency to every update
  WiFi.setAutoReconnect(true);
  WiFi.begin();  // stored credentials, if any

  ws.begin();
  ws.onEvent(onWsEvent);
  bootMs = millis();
}

static void netLoop() {
  if (netUi.portal) {
    wm.process();
    if (WiFi.status() == WL_CONNECTED) {
      wm.stopConfigPortal();
      netUi.portal = false;
    }
  }

  bool up = WiFi.status() == WL_CONNECTED;
  if (up != netUi.wifi) {
    netUi.wifi = up;
    if (up) {
      strlcpy(netUi.ip, WiFi.localIP().toString().c_str(), sizeof netUi.ip);
      strlcpy(netUi.ssid, WiFi.SSID().c_str(), sizeof netUi.ssid);
      if (!mdnsUp && MDNS.begin(netUi.hostname)) {
        MDNS.addService("claudescreen", "tcp", WS_PORT);
        MDNS.addServiceTxt("claudescreen", "tcp", "id", (const char*)netUi.id);
        MDNS.addServiceTxt("claudescreen", "tcp", "name", (const char*)netUi.name);
        mdnsUp = true;
      }
    }
  }
  if (otaUp && millis() > otaUntil && netUi.otaPct < 0) {  // update window closed unused
    ArduinoOTA.end();
    otaUp = false;
  }

  // No WiFi configured and no PC on USB: offer the phone setup network.
  if (!up && !netUi.portal && !wm.getWiFiIsSaved() && !usbSeen() && millis() - bootMs > 20000) startPortal();

  if (netUi.pairing && millis() > netUi.pairUntil) endPairing();
  ws.loop();
  if (otaUp) ArduinoOTA.handle();
}
