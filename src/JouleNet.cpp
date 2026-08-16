// ---------------------------------------------------------------------------
// JouleSuite for ESP32 / ESP8266 — JouleOTA · JouleSerial · JouleNet · JouleDash
// Author: Chinmoy Bhuyan
// Email:  dikibhuyan@gmail.com
// (c) 2026 — MIT License
// ---------------------------------------------------------------------------

// JouleNet implementation. Highlights:
//
//   * NVS layout: namespace "joulenet". Keys:
//        n         (uint8)   - number of saved SSIDs
//        s0..sN    (string)  - SSID
//        p0..pN    (string)  - password
//        h0..hN    (bool)    - hidden
//        p_<key>   (string)  - user-defined parameter values
//        ip,gw,mk,dn,sta     - static IP block + flag
//        hostname, country, mdns
//
//     `n` is written LAST: a power cut mid-save can lose the newest slot,
//     but NVS never claims a slot that isn't there.
//
//   * DNS hijack: every query while the portal is up gets answered with the
//     SoftAP's IP. iOS / Android captive-portal probes follow that lead and
//     pop the setup page automatically.
//
//   * autoConnect() returns immediately. The actual connect attempt runs
//     inside loop() via a small state machine: one WiFi.begin() per saved
//     network, each given _connectTimeoutMs to associate, advanced on
//     timeout. blockingConnect() wraps this in a delay loop for users who
//     want the old synchronous API.
//
//   * TASK SPLIT. HTTP handlers run on the AsyncTCP task; everything else
//     (sweep, portal watchdog, NVS writes, user callbacks) runs on the
//     Arduino task from loop(). A POST therefore only records what it wants
//     — _pcPending for /wifi/connect, _paramsDirty for /wifi/params — and
//     returns; loop() does the flash write and fires the callback. The one
//     thing a handler mutates directly is p.value, under paramMx().

#include "JouleNet.h"
#include "JouleNet_ui_gz.h"
#include <ArduinoJson.h>
#include <esp_wifi.h>   // esp_wifi_set_country / wifi_country_t (country code)

// Serve pre-compressed UI with Content-Encoding: gzip. 82,969 B → 28,498 B
// on the wire, reliable on weak Wi-Fi links where the uncompressed
// response would stall mid-stream.
static void sendGzippedUi(AsyncWebServerRequest *req, const uint8_t *gz, size_t len) {
  AsyncWebServerResponse *res = req->beginResponse(200, "text/html; charset=utf-8", gz, len);
  res->addHeader("Content-Encoding", "gzip");
  res->addHeader("Cache-Control", "public, max-age=3600");
  req->send(res);
}

// Largest legitimate body on any /wifi route is a handful of short strings.
// ESPAsyncWebServer imposes no limit of its own — it hands over whatever the
// client declared — so without a cap an anonymous POST can walk the heap up
// until AsyncTCP and the Wi-Fi stack have nothing left to allocate.
static const size_t kMaxBodyBytes = 4096;

// Per-request body accumulator. It lives in request->_tempObject, which the
// request frees with free(), so it has to stay POD — no String, no
// destructor. A function-local `static String` (the obvious shortcut) is one
// buffer for the whole server: two clients POSTing at once interleave their
// chunks and each parses a splice of the other's body.
struct BodyBuf { uint32_t cap; uint32_t len; char data[1]; };

// Returns the NUL-terminated body once it is complete, else nullptr — either
// because more is coming, or because the request was refused (in which case
// the response has already been sent).
static char *collectBody(AsyncWebServerRequest *req, const uint8_t *data,
                         size_t len, size_t index, size_t total) {
  if (index == 0) {
    if (req->_tempObject) { free(req->_tempObject); req->_tempObject = nullptr; }
    // Chunked requests arrive with total == 0 (no Content-Length to trust),
    // so they get the cap as their allocation.
    size_t cap = total ? total : kMaxBodyBytes;
    if (cap > kMaxBodyBytes) { req->send(413, "text/plain", "body too large"); return nullptr; }
    BodyBuf *b = (BodyBuf *)malloc(sizeof(BodyBuf) + cap);
    if (!b) { req->send(507, "text/plain", "out of memory"); return nullptr; }
    b->cap = cap; b->len = 0;
    req->_tempObject = b;
  }
  BodyBuf *b = (BodyBuf *)req->_tempObject;
  if (!b) return nullptr;                       // refused at index 0, or a stray chunk
  if (len) {
    if (b->len + len > b->cap) {
      free(b); req->_tempObject = nullptr;
      req->send(413, "text/plain", "body too large");
      return nullptr;
    }
    memcpy(b->data + b->len, data, len);
    b->len += len;
  }
  // A chunked body announces its end with a zero-length final callback; a
  // Content-Length body ends when the declared byte count has arrived.
  bool done = total ? (index + len >= total) : (len == 0);
  if (!done) return nullptr;
  b->data[b->len] = '\0';
  return b->data;
}

namespace joule {

static Preferences gNvs;
static const char *NS = "joulenet";
static const size_t kMaxNetworks = 8;   // NVS slots s0..s7 / p0..p7 / h0..h7

// Header / Divider / Display are presentation-only. Their content is owned
// by the firmware and recomputed on every boot (a section title, a rule, or
// a read-only value such as a device fingerprint). They must NOT be restored
// from / persisted to NVS, nor written back on a /wifi/params POST — doing so
// would let a stale persisted blank clobber a freshly-computed value. Only
// editable input types round-trip through storage and save.
static bool isInputParam(NetParamType t) {
  return t != NetParamType::Header &&
         t != NetParamType::Divider &&
         t != NetParamType::Display;
}

// WHICH TASK TOUCHES WHAT — this is the library's one small-string lock.
//   _params spine (size, key, label, type)  - built by addParameter() before
//                                             begin(), Arduino task only.
//   p.value                                 - written by the /wifi/params POST
//                                             (AsyncTCP task), read by
//                                             paramValue() (Arduino task), by
//                                             the GET handler (AsyncTCP) and by
//                                             _saveToNvs() (Arduino task).
//   _activeSsid                             - written by _beginAttempt() once
//                                             per sweep attempt (Arduino task),
//                                             read by _statusJson() and
//                                             activeSsid() (either task).
//   _hostname                               - written by _applyPendingConnect()
//                                             (Arduino task), read by
//                                             _statusJson() (AsyncTCP task).
// String::operator= frees the old buffer, so a cross-task read of any of these
// is a use-after-free. /wifi/status polls every 3 s while the sweep is
// reassigning _activeSsid, so this is the hot one. Every touch takes this
// mutex. NEVER hold it across something that blocks — flash (putString),
// Serial, WiFi calls, req->send(): copy under the lock, act after releasing it.
//
// One mutex for all of them: these are microsecond-long String copies on two
// tasks, so there is no contention worth splitting.
static SemaphoreHandle_t paramMx() {
  static SemaphoreHandle_t m = xSemaphoreCreateMutex();
  return m;
}
struct ParamLock {
  ParamLock()  { xSemaphoreTake(paramMx(), portMAX_DELAY); }
  ~ParamLock() { xSemaphoreGive(paramMx()); }
};

JouleNetClass::JouleNetClass() {}

void JouleNetClass::setApCredentials(const String &ssid, const String &password) {
  _apSsid = ssid; _apPass = password;
}

void JouleNetClass::setStaticIP(IPAddress ip, IPAddress gw, IPAddress mask, IPAddress dns) {
  _staticOn = true; _ip = ip; _gw = gw; _mask = mask; _dnsIp = dns;
  _staticFromCode = true;
}
void JouleNetClass::clearStaticIP() { _staticOn = false; _staticFromCode = true; }

void JouleNetClass::addParameter(const NetParam &p) {
  for (auto &x : _params) if (x.key == p.key) { x = p; return; }
  _params.push_back(p);
}

bool JouleNetClass::saveCredentials(const String &ssid, const String &password, bool hidden) {
  // Merge NVS in FIRST. _saveToNvs() would otherwise do it at the end, after
  // the capacity check below had already passed against a pre-begin() _saved
  // that only looks empty: the merge then pushes the total past kMaxNetworks,
  // the capped write loop drops the overflow, and we have returned true for a
  // save that silently evicted a persisted network. Loading here also lets the
  // dupe scan see persisted SSIDs, so re-seeding one updates it in place.
  if (!_nvsLoaded) _loadFromNvs();
  for (auto &n : _saved) if (n.ssid == ssid) { n.pass = password; n.hidden = hidden; _saveToNvs(); return true; }
  // Refuse rather than accept a network that works until the next reboot:
  // only kMaxNetworks slots exist in NVS, and a technician who provisioned
  // site 9 through the portal would have no way to tell it didn't stick.
  if (_saved.size() >= kMaxNetworks) return false;
  _saved.push_back({ssid, password, hidden});
  _saveToNvs();
  return true;
}

void JouleNetClass::clearAllCredentials() {
  // Load BEFORE clearing. _saveToNvs() reloads whenever !_nvsLoaded, so a
  // pre-begin() call (factory-reset jumper read in setup(), typically) would
  // re-populate _saved from the very slots being erased and write every one of
  // them straight back — the wipe reports success and the device rejoins the
  // old site after reboot. Loading first makes _nvsLoaded true, so the clear
  // is what reaches flash: n = 0.
  if (!_nvsLoaded) _loadFromNvs();
  _saved.clear(); _saveToNvs();
}

void JouleNetClass::_loadFromNvs() {
  gNvs.begin(NS, true);
  uint8_t n = gNvs.getUChar("n", 0);
  for (uint8_t i = 0; i < n && i < kMaxNetworks; i++) {
    NetCreds c;
    char k[8];
    snprintf(k,8,"s%u",i); c.ssid   = gNvs.getString(k, "");
    snprintf(k,8,"p%u",i); c.pass   = gNvs.getString(k, "");
    snprintf(k,8,"h%u",i); c.hidden = gNvs.getBool(k, false);
    if (!c.ssid.length()) continue;
    // A sketch may have seeded the same SSIDs with saveCredentials() before
    // begin(); appending blindly would double the list on every boot and
    // double the boot-time spent failing on dead networks.
    // …and the append stops at kMaxNetworks whatever the caller seeded, so the
    // list in RAM can never describe more networks than _saveToNvs() is able
    // to write back.
    bool dupe = false;
    for (auto &x : _saved) if (x.ssid == c.ssid) { dupe = true; break; }
    if (!dupe && _saved.size() < kMaxNetworks) _saved.push_back(c);
  }
  // Values the sketch set explicitly are its identity — see the setters.
  if (!_hostnameFromCode && gNvs.isKey("hostname")) _hostname    = gNvs.getString("hostname", _hostname);
  if (!_countryFromCode  && gNvs.isKey("country"))  _countryCode = gNvs.getString("country", _countryCode);
  if (!_mdnsFromCode     && gNvs.isKey("mdns"))     _mdnsName    = gNvs.getString("mdns", _mdnsName);
  if (!_staticFromCode   && gNvs.isKey("sta")) {
    _staticOn = gNvs.getBool("sta", false);
    if (_staticOn) {
      _ip     = IPAddress((uint32_t)gNvs.getUInt("ip", 0));
      _gw     = IPAddress((uint32_t)gNvs.getUInt("gw", 0));
      _mask   = IPAddress((uint32_t)gNvs.getUInt("mk", 0));
      _dnsIp  = IPAddress((uint32_t)gNvs.getUInt("dn", 0));
    }
  }
  for (auto &p : _params) {
    if (!isInputParam(p.type)) continue;   // firmware-owned, not stored
    String key = String("p_") + p.key;
    if (!gNvs.isKey(key.c_str())) continue;
    String v = gNvs.getString(key.c_str(), p.value);   // flash read, unlocked
    { ParamLock l; p.value = v; }
  }
  gNvs.end();
  _nvsLoaded = true;
}

void JouleNetClass::_saveToNvs() {
  // A sketch may saveCredentials() before begin() — MultiSSID.ino seeds five
  // networks that way, and the quick start calls addParameter() before it.
  // Writing here without reading first would put the code defaults over every
  // persisted p_<key>, hostname, country and static block, and then begin()
  // would read back what this call just wrote. Load first so the write is a
  // merge, not a clobber.
  if (!_nvsLoaded) _loadFromNvs();
  gNvs.begin(NS, false);
  for (size_t i = 0; i < _saved.size() && i < kMaxNetworks; i++) {
    char k[8];
    snprintf(k,8,"s%u",(unsigned)i); gNvs.putString(k, _saved[i].ssid);
    snprintf(k,8,"p%u",(unsigned)i); gNvs.putString(k, _saved[i].pass);
    snprintf(k,8,"h%u",(unsigned)i); gNvs.putBool  (k, _saved[i].hidden);
  }
  gNvs.putString("hostname", _hostname);
  gNvs.putString("country",  _countryCode);
  gNvs.putString("mdns",     _mdnsName);
  gNvs.putBool  ("sta",      _staticOn);
  if (_staticOn) {
    gNvs.putUInt("ip", (uint32_t)_ip);
    gNvs.putUInt("gw", (uint32_t)_gw);
    gNvs.putUInt("mk", (uint32_t)_mask);
    gNvs.putUInt("dn", (uint32_t)_dnsIp);
  }
  for (auto &p : _params) {
    if (!isInputParam(p.type)) continue;   // firmware-owned, not stored
    String key = String("p_") + p.key;
    String v; { ParamLock l; v = p.value; }   // copy under the lock…
    gNvs.putString(key.c_str(), v);          // …write flash without it
  }
  // Count last, and never more than the slots that exist: a power cut here
  // costs the newest network, whereas writing it first would leave NVS
  // describing slots whose SSID and password belong to different networks.
  size_t n = _saved.size() < kMaxNetworks ? _saved.size() : kMaxNetworks;
  gNvs.putUChar("n", (uint8_t)n);
  gNvs.end();
}

// ---------- portal handlers -------------------------------------------------

String JouleNetClass::_statusJson() const {
  JsonDocument d;
  // Runs on the AsyncTCP task. Snapshot the two Strings the Arduino task
  // reassigns before ArduinoJson copies characters out of their heap buffers —
  // see paramMx(). _title / _brandColor / _mdnsName are set before begin() and
  // never touched again, so they need no lock.
  String ssid, host;
  { ParamLock lk; ssid = _activeSsid; host = _hostname; }
  // title + brand let the browser-side JS apply setTitle() / setBrandColor()
  // at runtime even though the HTML body itself ships pre-gzipped (no
  // server-side template substitution path).
  d["title"]    = _title;
  d["brand"]    = _brandColor;
  d["state"]    = (int)_state;
  d["ssid"]     = ssid;
  d["ip"]       = WiFi.localIP().toString();
  d["gateway"]  = WiFi.gatewayIP().toString();
  d["mask"]     = WiFi.subnetMask().toString();
  d["dns"]      = WiFi.dnsIP().toString();
  d["bssid"]    = WiFi.BSSIDstr();
  d["channel"]  = WiFi.channel();
  d["rssi"]     = WiFi.RSSI();
  d["hostname"] = host;
  d["mdns"]     = _mdnsName + ".local";
  d["mac"]      = WiFi.macAddress();
  d["heap"]     = ESP.getFreeHeap();
  d["uptime_s"] = millis()/1000;
  // Portal UX hints — the SPA reads these on first load to pick its opening
  // tab and to hide the Wi-Fi picker on AP-only setup appliances.
  d["uiDefaultTab"] = _uiDefaultTab;
  d["uiHideWifi"]   = _uiHideWifi;
  String s; serializeJson(d, s); return s;
}

bool JouleNetClass::_authGate(AsyncWebServerRequest *req) {
  if (_authUser.length() == 0) return true;            // auth disabled (back-compat)
  if (!req->authenticate(_authUser.c_str(), _authPass.c_str())) {
    req->requestAuthentication();                      // 401 + WWW-Authenticate
    return false;
  }
  return true;
}

void JouleNetClass::_mountHandlers() {
  // Captive-portal "is this the internet?" probes need a 200/204 from any
  // host. We serve the portal for /, and a small detect file for known
  // probe URLs (so the OS pops the page automatically).
  auto sendPortal = [this](AsyncWebServerRequest *req){
    if (!_authGate(req)) return;
    sendGzippedUi(req, joule::NET_UI_HTML_GZ, joule::NET_UI_HTML_GZ_LEN);
  };
  // All portal-pop URLs must be EXACT match. The default
  // BackwardCompatible matcher would make `/wifi` swallow `/wifi/*`,
  // and `/` would swallow every request on the server. Routes registered
  // with another library (JouleDash on `/`) won't even get a chance unless
  // we constrain ourselves with exact().
  _server->on(AsyncURIMatcher::exact("/wifi"), HTTP_GET, sendPortal);
  _server->on(AsyncURIMatcher::exact("/generate_204"), HTTP_GET, sendPortal);   // Android
  _server->on(AsyncURIMatcher::exact("/gen_204"),      HTTP_GET, sendPortal);
  _server->on(AsyncURIMatcher::exact("/hotspot-detect.html"), HTTP_GET, sendPortal); // iOS/macOS
  _server->on(AsyncURIMatcher::exact("/ncsi.txt"),     HTTP_GET, sendPortal);   // Windows
  // NB: deliberately NOT mounting "/" — that belongs to whichever library
  // the host sketch chose as its primary UI (typically JouleDash). Portal
  // pop still works via the OS-specific probe URLs above.

  // Visible SSIDs and their BSSIDs describe the site, not just this device —
  // gated with everything else when setAuth() is configured.
  _server->on("/wifi/scan", HTTP_GET, [this](AsyncWebServerRequest *req){
    if (!_authGate(req)) return;
    int n = WiFi.scanComplete();
    JsonDocument d; auto arr = d["networks"].to<JsonArray>();
    for (int i = 0; i < n; i++) {
      auto o = arr.add<JsonObject>();
      o["ssid"]  = WiFi.SSID(i);
      o["rssi"]  = WiFi.RSSI(i);
      o["ch"]    = WiFi.channel(i);
      o["bssid"] = WiFi.BSSIDstr(i);
      o["sec"]   = (int)WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    }
    // Serve the cached results and only refresh on a cooldown. A scan takes
    // the radio off the SoftAP channel for seconds — rescanning per request
    // makes the portal page freeze exactly while it is being browsed — and
    // one started during an association attempt can abort it outright.
    if (n != WIFI_SCAN_RUNNING && _state != NetState::Connecting &&
        (_lastScanAt == 0 || (millis() - _lastScanAt) > 10000)) {
      WiFi.scanNetworks(true, true);
      _lastScanAt = millis() | 1;
    }
    String s; serializeJson(d, s);
    req->send(200, "application/json", s);
  });

  _server->on("/wifi/connect", HTTP_POST,
    [](AsyncWebServerRequest *req){ /* respond from body handler */ },
    NULL,
    [this](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total){
      // Gate BEFORE a byte is buffered: checking only on the last chunk lets
      // an anonymous client fill the heap with a body we were always going
      // to reject.
      if (index == 0 && !_authGate(req)) return;
      char *body = collectBody(req, data, len, index, total);
      if (!body) return;

      JsonDocument d;
      if (deserializeJson(d, body) != DeserializationError::Ok) {
        req->send(400, "text/plain", "bad json"); return;
      }
      String ssid = d["ssid"] | "";
      if (!ssid.length()) { req->send(400, "text/plain", "ssid required"); return; }

      String sip  = d["staticIp"] | "";
      String sgw  = d["gateway"]  | "";
      String smk  = d["netmask"]  | "";
      String sdns = d["dns"]      | "";
      int8_t staticMode = -1;                          // leave the config alone
      IPAddress a, b, c, e;
      if (sip.length() && sgw.length() && smk.length()) {
        // Persisting a half-parsed address is unrecoverable without a
        // physical NVS erase — the device would boot into a configuration it
        // can never associate with. Reject the whole request instead.
        if (!a.fromString(sip) || !b.fromString(sgw) || !c.fromString(smk) ||
            (sdns.length() && !e.fromString(sdns))) {
          req->send(400, "text/plain", "bad ip"); return;
        }
        if (!sdns.length()) e = b;   // no resolver given: the gateway is the safe guess
        staticMode = 1;
      } else if (d["dhcp"] | false) {
        staticMode = 0;
      }
      // NB: absent static fields mean "unchanged", not "clear". The portal
      // sends them empty whenever the Advanced panel is untouched, and a
      // technician moving the device to another SSID must not silently drop
      // the fixed address every dashboard bookmark points at. Clearing takes
      // an explicit {"dhcp":true} (or clearStaticIP() in code).

      // Hand the work to loop(): this callback runs on the AsyncTCP task,
      // where saving would reallocate _saved under the connect sweep, write
      // flash, and fire the user's onState() callback off-task.
      // One slot, one writer: refuse a second POST until loop() has taken the
      // first. Overwriting instead would free the SSID/password Strings that
      // _applyPendingConnect() is at that moment persisting to flash — a
      // double-tap on Connect would corrupt NVS, not merely lose an update.
      if (_pcPending) { req->send(409, "text/plain", "busy"); return; }
      _pcSsid   = ssid;
      _pcPass   = d["password"] | "";
      _pcHidden = d["hidden"] | false;
      _pcHost   = d["hostname"] | "";
      _pcCc     = d["countryCode"] | "";
      _pcIp = a; _pcGw = b; _pcMask = c; _pcDns = e;
      _pcStatic = staticMode;
      _pcPending = true;

      req->send(200, "text/plain", "ok");
    });

  _server->on("/wifi/params", HTTP_GET, [this](AsyncWebServerRequest *req){
    if (!_authGate(req)) return;
    JsonDocument d; auto arr = d["params"].to<JsonArray>();
    for (auto &p : _params) {
      auto o = arr.add<JsonObject>();
      o["key"]=p.key; o["label"]=p.label;
      o["type"] = (p.type==NetParamType::Password?"password":
                   p.type==NetParamType::Number?"number":
                   p.type==NetParamType::Toggle?"toggle":
                   p.type==NetParamType::Dropdown?"dropdown":
                   p.type==NetParamType::Color?"color":
                   p.type==NetParamType::Textarea?"textarea":
                   p.type==NetParamType::Display?"display":
                   p.type==NetParamType::Header?"header":
                   p.type==NetParamType::Divider?"divider":"text");
      // A Password parameter is a secret the firmware asked the operator to
      // type (a broker password, typically). The form still needs the field,
      // but never the stored value — an empty box means "unchanged" on save.
      // ArduinoJson copies the String, so the copy happens under the lock —
      // the /wifi/params POST reassigns p.value from the AsyncTCP task.
      { ParamLock lk; o["value"] = (p.type == NetParamType::Password) ? String() : p.value; }
      o["hint"]=p.hint; o["opts"]=p.opts;
      o["min"]=p.min; o["max"]=p.max;
    }
    String s; serializeJson(d, s); req->send(200,"application/json",s);
  });

  _server->on("/wifi/params", HTTP_POST,
    [](AsyncWebServerRequest *req){},
    NULL,
    [this](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total){
      if (index == 0 && !_authGate(req)) return;      // before buffering, see /wifi/connect
      char *body = collectBody(req, data, len, index, total);
      if (!body) return;

      JsonDocument d;
      if (deserializeJson(d, body) != DeserializationError::Ok) {
        req->send(400,"text/plain","bad json"); return;
      }
      for (auto &p : _params) {
        if (!isInputParam(p.type)) continue;          // presentation-only, never written
        if (d[p.key].isNull()) continue;
        // as<String>() stringifies whatever JSON type arrived: a quoted
        // string passes through verbatim, a JSON number ("number" inputs
        // bind to JS numbers, not strings) becomes its decimal text, a
        // bool becomes "true"/"false". The previous (const char*) cast
        // returned nullptr for any non-string variant, so editing a
        // Number field silently saved an empty value and it reverted to
        // default on reload.
        String v = d[p.key].as<String>();
        // The GET never hands out a stored password, so the form always
        // posts an empty one back unless the operator typed a new secret.
        // Ceiling: a password can be replaced from the portal but not
        // blanked — that takes /wifi/reset or a change in firmware.
        if (p.type == NetParamType::Password && !v.length()) continue;
        { ParamLock lk; p.value = v; }   // see paramMx(): p.value is cross-task
      }
      // This runs on the AsyncTCP task. Persisting here would open gNvs — the
      // one file-scope Preferences handle — underneath a saveCredentials()
      // that loop() may be running right now, and every putString() after the
      // other task's end() would silently fail against a closed handle. Flag
      // it instead; loop() writes flash and fires onConfig() on the Arduino
      // task, where every other callback already fires.
      _paramsDirty = true;
      req->send(200,"text/plain","ok");
    });

  // MAC, BSSID and the joined SSID are site topology — gated with the rest.
  _server->on("/wifi/status", HTTP_GET, [this](AsyncWebServerRequest *req){
    if (!_authGate(req)) return;
    req->send(200,"application/json",_statusJson());
  });
  // Reboot from loop(), not here: req->send() only *stores* the response —
  // AsyncTCP writes it after the handler returns, so restarting inline (or
  // delay()ing on the AsyncTCP task, which stalls every other connection)
  // means the client never sees the 200 and cannot tell if it was accepted.
  _server->on("/wifi/reset", HTTP_POST, [this](AsyncWebServerRequest *req){
    if(!_authGate(req)) return;
    req->send(200,"text/plain","ok");
    _rebootWipesNvs = true; _rebootAt = (millis() + 300) | 1;
  });
  _server->on("/wifi/restart", HTTP_POST, [this](AsyncWebServerRequest *req){
    if(!_authGate(req)) return;
    req->send(200,"text/plain","ok");
    _rebootWipesNvs = false; _rebootAt = (millis() + 300) | 1;
  });
}

// Push _countryCode into esp_wifi. Called from begin() and again whenever the
// portal changes it — setCountryCode() used to be a dead setter (the radio
// stayed on the default "01" worldwide map, restricting channels and TX
// power); e.g. "IN" enables ch 1-13.
void JouleNetClass::_applyCountry() {
  if (_countryCode.length() < 2) return;
  wifi_country_t ctry = {};
  ctry.cc[0] = _countryCode[0];
  ctry.cc[1] = _countryCode[1];
  ctry.cc[2] = '\0';
  ctry.schan = 1;
  ctry.nchan = (_countryCode == "01" || _countryCode == "US") ? 11 : 13;
  ctry.policy = WIFI_COUNTRY_POLICY_MANUAL;
  esp_wifi_set_country(&ctry);
}

void JouleNetClass::begin(AsyncWebServer *server) {
  _server = server;
  _loadFromNvs();
  // IMPORTANT: WiFi.setHostname() touches lwIP, which crashes
  // (`tcpip_api_call ... Invalid mbox`) if the netif hasn't been brought
  // up yet. Call mode() first to initialize the underlying esp_wifi + lwIP
  // glue before any other Wi-Fi method.
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(_hostname.c_str());
  _applyCountry();
  _mountHandlers();
}

void JouleNetClass::_beginAttempt(const NetCreds &c) {
  { ParamLock lk; _activeSsid = c.ssid; }   // read by /wifi/status, see paramMx()
  // Keep the SoftAP up while the portal is running: the phone that just
  // posted these credentials is still associated to it and polling
  // /wifi/status to find out how the attempt went.
  WiFi.mode(_dnsRunning ? WIFI_AP_STA : WIFI_STA);
  // Disable Wi-Fi modem-sleep + crank TX power to the radio max. On a weak
  // link (RSSI worse than -75 dBm) the default settings drop TCP ACKs and
  // AsyncTCP stalls partway through a chunked HTML response. The ~75mA
  // delta is fine on a mains-powered device; battery firmware should
  // revisit these.
  WiFi.setSleep(false);
  WiFi.setTxPower(WIFI_POWER_19_5dBm);   // 19.5 dBm ≈ 89 mW, the hardware max
  if (_staticOn) WiFi.config(_ip, _gw, _mask, _dnsIp);
  // Going back to DHCP takes an explicit all-zero config(): once a static
  // address has been applied, arduino-esp32 remembers it (_useStaticIp) and
  // WiFi.begin() no longer resets the netif, so the STA would come up on the
  // old fixed address with its DHCP client stopped — unroutable on the new
  // site, and only a power cycle would clear it. Handing config() 0.0.0.0 is
  // what restarts dhcpc (WiFiGeneric.cpp: `if (info.ip.addr == 0)`).
  else WiFi.config((uint32_t)0, (uint32_t)0, (uint32_t)0);
  WiFi.begin(c.ssid.c_str(), c.pass.c_str(), 0, NULL, true);
}

void JouleNetClass::_onLinkUp() {
  _sweeping = false;
  _attemptStartedAt = 0;
  _disconnectAt = 0;
  if (_dnsRunning) stopPortal();        // we have a station link; drop the AP
  MDNS.end(); MDNS.begin(_mdnsName.c_str()); MDNS.addService("http","tcp",80);
  _setState(NetState::Connected);
}

void JouleNetClass::_startSweep(size_t firstIdx) {
  if (_saved.empty()) return;
  _sweepIdx = firstIdx < _saved.size() ? firstIdx : 0;
  _sweeping = true;
  _attemptStartedAt = 0;                // the next _pumpConnect() issues WiFi.begin()
  _setState(NetState::Connecting);
}

// One tick of the connect sweep: at most one WiFi.begin() per saved network,
// each given _connectTimeoutMs to associate. Blocking here instead (the old
// delay(80) spin) stalls the sketch for N × timeout — two minutes with a
// full saved list — and stalls it again every time the portal times out.
void JouleNetClass::_pumpConnect() {
  if (!_sweeping) return;
  if (WiFi.status() == WL_CONNECTED) { _onLinkUp(); return; }

  if (_attemptStartedAt == 0) {
    if (_sweepIdx >= _saved.size()) {   // nothing left to try
      _sweeping = false;
      // A sweep started from the portal (wrong password, typically) drops
      // back into Portal rather than tearing the AP down and rebuilding it —
      // the operator is still on that page about to try again.
      if (_dnsRunning) {
        _portalStartedAt = millis();     // give them the full timeout to retry
        _setState(NetState::Portal);
        return;
      }
      _setState(NetState::Failed);
      startPortal();
      _portalAuto = true;   // library-raised: loop() may close it if the link returns
      return;
    }
    _beginAttempt(_saved[_sweepIdx]);
    _attemptStartedAt = millis() | 1;
    return;
  }
  if ((millis() - _attemptStartedAt) > _connectTimeoutMs) {
    _sweepIdx++;
    _attemptStartedAt = 0;              // next tick starts the next network
  }
}

// Applies a /wifi/connect request on the Arduino task. The AsyncTCP handler
// answers 409 while _pcPending is set, so the snapshot below cannot be
// rewritten under us; nothing past this point reads _pc* directly, because
// saveCredentials() spends milliseconds inside a flash write and a String
// reassigned by the other task mid-write would persist garbage.
void JouleNetClass::_applyPendingConnect() {
  const String  ssid = _pcSsid, pass = _pcPass, host = _pcHost, cc = _pcCc;
  const bool    hidden = _pcHidden;
  const int8_t  stat   = _pcStatic;
  const IPAddress ip = _pcIp, gw = _pcGw, mask = _pcMask, dns = _pcDns;
  _pcPending = false;                 // only now may the next POST land

  // Assign under the lock, then hand WiFi our own local copy — holding it
  // across setHostname() would block the AsyncTCP task inside lwIP.
  if (host.length()) { { ParamLock lk; _hostname = host; } WiFi.setHostname(host.c_str()); }
  if (cc.length()) {
    _countryCode = cc;
    // Push it into the radio now, not just into NVS: the operator set the
    // country *because* the AP they are about to join is on ch 12/13, and
    // esp_wifi would otherwise stay on the "01" 1-11 map until a reboot.
    _applyCountry();
  }
  if      (stat == 1) setStaticIP(ip, gw, mask, dns);
  else if (stat == 0) clearStaticIP();

  if (!saveCredentials(ssid, pass, hidden)) {
    // All 8 slots are taken. The 200 has already gone out, so surface it the
    // only way the SPA still watches: the Status tab.
    _setState(NetState::Failed);
    return;
  }
  // Someone is provisioning right now; the "we've been offline too long"
  // timer that may have raised this portal starts over.
  _disconnectAt = 0;
  // Try the network the operator just picked first — they are standing in
  // front of the device waiting for it.
  size_t idx = 0;
  for (size_t i = 0; i < _saved.size(); i++) if (_saved[i].ssid == ssid) { idx = i; break; }
  _startSweep(idx);
}

void JouleNetClass::autoConnect() {
  if (_saved.empty()) { startPortal(); return; }
  _startSweep(0);
}

bool JouleNetClass::blockingConnect(uint32_t timeoutMs) {
  autoConnect();
  // timeoutMs is the whole budget. The sweep keeps running in the sketch's
  // own loop() afterwards, so a false here means "not yet", not "given up".
  uint32_t start = millis();
  while ((millis() - start) < timeoutMs && _state == NetState::Connecting) { delay(50); loop(); }
  return _state == NetState::Connected;
}

void JouleNetClass::_startSoftAp() {
  WiFi.mode(WIFI_AP_STA);
  if (_apPass.length()) WiFi.softAP(_apSsid.c_str(), _apPass.c_str());
  else WiFi.softAP(_apSsid.c_str());
  delay(120);
  if (!_dnsRunning) {
    _dns.setErrorReplyCode(DNSReplyCode::NoError);
    _dns.start(53, "*", WiFi.softAPIP());
    _dnsRunning = true;
  }
}

void JouleNetClass::startPortal() {
  _startSoftAp();
  // Disarm the outage timer. Leaving it armed made the reprovision watchdog
  // fire again the instant the portal came down — stopPortal() and the portal
  // timeout both became no-ops, and a field device that lost Wi-Fi broadcast
  // its SoftAP forever. The watchdog re-arms on the next Connected→dropped
  // transition; _portalAuto, not this timestamp, records who raised the
  // portal. Callers inside the library set _portalAuto right after this
  // returns; a sketch calling startPortal() gets false, i.e. a portal that
  // stays up until it says otherwise.
  _disconnectAt = 0;
  _portalAuto   = false;
  _portalStartedAt = millis();
  _setState(NetState::Portal);
  // Kick a scan now so the first /wifi/scan the phone makes has a list to
  // show instead of an empty picker the operator has to refresh by hand.
  if (WiFi.scanComplete() != WIFI_SCAN_RUNNING) {
    WiFi.scanNetworks(true, true);
    _lastScanAt = millis() | 1;
  }
}

void JouleNetClass::stopPortal() {
  _dns.stop(); _dnsRunning = false; _portalAuto = false;
  WiFi.softAPdisconnect(true);
  // Leave the state consistent: getState() reporting Portal after the portal
  // is gone means the sketch never sees Connected again.
  if (_state == NetState::Portal)
    _setState(WiFi.status() == WL_CONNECTED ? NetState::Connected : NetState::Idle);
}

void JouleNetClass::resetAndReboot() {
  gNvs.begin(NS, false); gNvs.clear(); gNvs.end();
  delay(200); ESP.restart();
}

void JouleNetClass::loop() {
  if (_dnsRunning) _dns.processNextRequest();

  if (_rebootAt && (int32_t)(millis() - _rebootAt) >= 0) {
    if (_rebootWipesNvs) resetAndReboot();
    ESP.restart();
  }

  if (_pcPending) _applyPendingConnect();

  // /wifi/params landed on the AsyncTCP task and only set a flag; the flash
  // write and the sketch's callback belong here, on the Arduino task.
  if (_paramsDirty) {
    _paramsDirty = false;
    _saveToNvs();
    if (_onConfig) {
      // Hand the callback a snapshot: the live vector's Strings can be
      // reassigned by the next POST while the sketch is reading them.
      std::vector<NetParam> snap;
      { ParamLock lk; snap = _params; }
      _onConfig(snap);
    }
  }

  // Portal timeout: drop AP after a while of nobody using it so the radio
  // returns to pure STA mode.
  if (_state == NetState::Portal && _portalTimeoutMs > 0 &&
      (millis() - _portalStartedAt) > _portalTimeoutMs && !_saved.empty()) {
    stopPortal(); autoConnect();
  }

  _pumpConnect();

  if (_state == NetState::Connected) {
    if (WiFi.status() != WL_CONNECTED) {
      _setState(NetState::Connecting);
      // millis()|1 keeps 0 meaning "no outage pending" — the rollover tick
      // would otherwise disarm the reprovision watchdog for 49.7 days.
      _disconnectAt = millis() | 1;
      if (_autoReconnect) WiFi.reconnect();
    }
  } else if (_state == NetState::Connecting && WiFi.status() == WL_CONNECTED) {
    _onLinkUp();
  } else if (_state == NetState::Portal && _portalAuto && WiFi.status() == WL_CONNECTED) {
    // The STA interface stays up under the portal (WIFI_AP_STA), so the link
    // can come back on its own — after a router reboot, say — while the
    // watchdog-raised portal is still broadcasting. Nothing else notices it,
    // and the SoftAP would stay open forever. Only portals the watchdog
    // raised close this way; a startPortal() the sketch asked for stays up
    // until it says otherwise.
    _onLinkUp();
  }

  // Reprovision watchdog: if we've been disconnected for too long, pop the
  // portal so a tech in front of the device can re-onboard it.
  if (_disconnectAt && _reprovisionMs &&
      (millis() - _disconnectAt) > _reprovisionMs &&
      _state != NetState::Portal) {
    startPortal();        // clears _disconnectAt, so this fires once per outage
    _portalAuto = true;   // …and this portal may close itself if the link returns
  }
}

void JouleNetClass::_setState(NetState s) {
  if (_state == s) return;
  _state = s;
  if (_onState) _onState(s);
}

IPAddress JouleNetClass::localIP()    const { return WiFi.localIP(); }
IPAddress JouleNetClass::gatewayIP()  const { return WiFi.gatewayIP(); }
IPAddress JouleNetClass::subnetMask() const { return WiFi.subnetMask(); }
IPAddress JouleNetClass::apIP()       const { return WiFi.softAPIP(); }
String    JouleNetClass::bssid()      const { return WiFi.BSSIDstr(); }
int       JouleNetClass::rssi()       const { return WiFi.RSSI(); }
uint8_t   JouleNetClass::channel()    const { return WiFi.channel(); }
String    JouleNetClass::activeSsid() const { ParamLock lk; return _activeSsid; }
String    JouleNetClass::paramValue(const String &key) const {
  // Called from the sketch's loop(); the /wifi/params POST reassigns p.value
  // on the AsyncTCP task. Copy under the lock — see paramMx().
  ParamLock lk;
  for (auto &p : _params) if (p.key == key) return p.value;
  return String();
}

} // namespace joule

joule::JouleNetClass JouleNet;
