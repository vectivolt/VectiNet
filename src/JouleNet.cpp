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
//        param.*   (string)  - user-defined parameter values
//        ip,gw,mk,dn,sta     - static IP block + flag
//        hostname, country, mdns
//
//   * DNS hijack: every query while the portal is up gets answered with the
//     SoftAP's IP. iOS / Android captive-portal probes follow that lead and
//     pop the setup page automatically.
//
//   * autoConnect() returns immediately. The actual connect attempt runs
//     inside loop() via a small state machine. blockingConnect() wraps this
//     in a delay loop for users who want the old synchronous API.

#include "JouleNet.h"
#include "JouleNet_ui.h"
#include "JouleNet_ui_gz.h"
#include <ArduinoJson.h>

// Serve pre-compressed UI with Content-Encoding: gzip. ~11 KB → ~3.7 KB
// on the wire, reliable on weak Wi-Fi links where the uncompressed
// response would stall mid-stream.
static void sendGzippedUi(AsyncWebServerRequest *req, const uint8_t *gz, size_t len) {
  AsyncWebServerResponse *res = req->beginResponse(200, "text/html; charset=utf-8", gz, len);
  res->addHeader("Content-Encoding", "gzip");
  res->addHeader("Cache-Control", "public, max-age=3600");
  req->send(res);
}

namespace joule {

#if defined(ESP32)
static Preferences gNvs;
static const char *NS = "joulenet";
#endif

JouleNetClass::JouleNetClass() {}

static String renderHtml(const String &title, const String &brand) {
  String s = FPSTR(NET_UI_HTML);
  s.replace("__TITLE__", title);
  s.replace("__BRAND__", brand);
  return s;
}

void JouleNetClass::setApCredentials(const String &ssid, const String &password) {
  _apSsid = ssid; _apPass = password;
}

void JouleNetClass::setStaticIP(IPAddress ip, IPAddress gw, IPAddress mask, IPAddress dns) {
  _staticOn = true; _ip = ip; _gw = gw; _mask = mask; _dnsIp = dns;
}
void JouleNetClass::clearStaticIP() { _staticOn = false; }

void JouleNetClass::addParameter(const NetParam &p) {
  for (auto &x : _params) if (x.key == p.key) { x = p; return; }
  _params.push_back(p);
}

void JouleNetClass::saveCredentials(const String &ssid, const String &password, bool hidden) {
  for (auto &n : _saved) if (n.ssid == ssid) { n.pass = password; n.hidden = hidden; _saveToNvs(); return; }
  _saved.push_back({ssid, password, hidden});
  _saveToNvs();
}

void JouleNetClass::clearAllCredentials() {
  _saved.clear(); _saveToNvs();
}

void JouleNetClass::_loadFromNvs() {
#if defined(ESP32)
  gNvs.begin(NS, true);
  uint8_t n = gNvs.getUChar("n", 0);
  for (uint8_t i = 0; i < n && i < 8; i++) {
    NetCreds c;
    char k[8];
    snprintf(k,8,"s%u",i); c.ssid   = gNvs.getString(k, "");
    snprintf(k,8,"p%u",i); c.pass   = gNvs.getString(k, "");
    snprintf(k,8,"h%u",i); c.hidden = gNvs.getBool(k, false);
    if (c.ssid.length()) _saved.push_back(c);
  }
  if (gNvs.isKey("hostname"))  _hostname    = gNvs.getString("hostname", _hostname);
  if (gNvs.isKey("country"))   _countryCode = gNvs.getString("country", _countryCode);
  if (gNvs.isKey("mdns"))      _mdnsName    = gNvs.getString("mdns", _mdnsName);
  if (gNvs.isKey("sta")) {
    _staticOn = gNvs.getBool("sta", false);
    if (_staticOn) {
      _ip     = IPAddress((uint32_t)gNvs.getUInt("ip", 0));
      _gw     = IPAddress((uint32_t)gNvs.getUInt("gw", 0));
      _mask   = IPAddress((uint32_t)gNvs.getUInt("mk", 0));
      _dnsIp  = IPAddress((uint32_t)gNvs.getUInt("dn", 0));
    }
  }
  for (auto &p : _params) {
    String key = String("p_") + p.key;
    if (gNvs.isKey(key.c_str())) p.value = gNvs.getString(key.c_str(), p.value);
  }
  gNvs.end();
#endif
}

void JouleNetClass::_saveToNvs() {
#if defined(ESP32)
  gNvs.begin(NS, false);
  gNvs.putUChar("n", (uint8_t)_saved.size());
  for (size_t i = 0; i < _saved.size() && i < 8; i++) {
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
    String key = String("p_") + p.key;
    gNvs.putString(key.c_str(), p.value);
  }
  gNvs.end();
#endif
}

// ---------- portal handlers -------------------------------------------------

String JouleNetClass::_statusJson() const {
  JsonDocument d;
  d["state"]    = (int)_state;
  d["ssid"]     = _activeSsid;
  d["ip"]       = WiFi.localIP().toString();
  d["gateway"]  = WiFi.gatewayIP().toString();
  d["mask"]     = WiFi.subnetMask().toString();
  d["dns"]      = WiFi.dnsIP().toString();
  d["bssid"]    = WiFi.BSSIDstr();
  d["channel"]  = WiFi.channel();
  d["rssi"]     = WiFi.RSSI();
  d["hostname"] = _hostname;
  d["mdns"]     = _mdnsName + ".local";
  d["mac"]      = WiFi.macAddress();
  d["heap"]     = ESP.getFreeHeap();
  d["uptime_s"] = millis()/1000;
  String s; serializeJson(d, s); return s;
}

void JouleNetClass::_mountHandlers() {
  // Captive-portal "is this the internet?" probes need a 200/204 from any
  // host. We serve the portal for /, and a small detect file for known
  // probe URLs (so the OS pops the page automatically).
  auto sendPortal = [this](AsyncWebServerRequest *req){
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

  _server->on("/wifi/scan", HTTP_GET, [this](AsyncWebServerRequest *req){
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_FAILED || n == -2) { WiFi.scanNetworks(true, true); n = 0; }
    if (n == WIFI_SCAN_RUNNING) n = 0;
    JsonDocument d; auto arr = d["networks"].to<JsonArray>();
    for (int i = 0; i < n; i++) {
      auto o = arr.add<JsonObject>();
      o["ssid"]  = WiFi.SSID(i);
      o["rssi"]  = WiFi.RSSI(i);
      o["ch"]    = WiFi.channel(i);
      o["bssid"] = WiFi.BSSIDstr(i);
      o["sec"]   = (int)WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    }
    if (n) WiFi.scanDelete();
    // Trigger the next scan async so the next request has fresh data.
    WiFi.scanNetworks(true, true);
    String s; serializeJson(d, s);
    req->send(200, "application/json", s);
  });

  _server->on("/wifi/connect", HTTP_POST,
    [](AsyncWebServerRequest *req){ /* respond from body handler */ },
    NULL,
    [this](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total){
      static String buf;
      if (index == 0) buf = "";
      buf.concat((const char*)data, len);
      if (index + len == total) {
        JsonDocument d;
        if (deserializeJson(d, buf) != DeserializationError::Ok) {
          req->send(400, "text/plain", "bad json"); return;
        }
        String ssid = d["ssid"] | "";
        String pass = d["password"] | "";
        bool hidden = d["hidden"] | false;
        if (!ssid.length()) { req->send(400, "text/plain", "ssid required"); return; }

        String host = d["hostname"] | "";
        String cc   = d["countryCode"] | "";
        String sip  = d["staticIp"] | "";
        String sgw  = d["gateway"]  | "";
        String smk  = d["netmask"]  | "";

        if (host.length()) _hostname = host;
        if (cc.length())   _countryCode = cc;
        if (sip.length() && sgw.length() && smk.length()) {
          IPAddress a,b,c; a.fromString(sip); b.fromString(sgw); c.fromString(smk);
          setStaticIP(a,b,c);
        } else clearStaticIP();

        saveCredentials(ssid, pass, hidden);
        req->send(200, "text/plain", "ok");

        // Try to connect on the next loop() tick.
        _setState(NetState::Connecting);
        _disconnectAt = 0;
      }
    });

  _server->on("/wifi/params", HTTP_GET, [this](AsyncWebServerRequest *req){
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
                   p.type==NetParamType::Header?"header":
                   p.type==NetParamType::Divider?"divider":"text");
      o["value"]=p.value; o["hint"]=p.hint; o["opts"]=p.opts;
      o["min"]=p.min; o["max"]=p.max;
    }
    String s; serializeJson(d, s); req->send(200,"application/json",s);
  });

  _server->on("/wifi/params", HTTP_POST,
    [](AsyncWebServerRequest *req){},
    NULL,
    [this](AsyncWebServerRequest *req, uint8_t *data, size_t len, size_t index, size_t total){
      static String buf;
      if (index == 0) buf = "";
      buf.concat((const char*)data, len);
      if (index + len == total) {
        JsonDocument d;
        if (deserializeJson(d, buf) != DeserializationError::Ok) {
          req->send(400,"text/plain","bad json"); return;
        }
        for (auto &p : _params) {
          if (d[p.key].is<const char*>()) p.value = String((const char*)d[p.key]);
          else if (!d[p.key].isNull()) p.value = String((const char*)d[p.key]);
        }
        _saveToNvs();
        if (_onConfig) _onConfig(_params);
        req->send(200,"text/plain","ok");
      }
    });

  _server->on("/wifi/status",   HTTP_GET,  [this](AsyncWebServerRequest *req){ req->send(200,"application/json",_statusJson()); });
  _server->on("/wifi/reset",    HTTP_POST, [this](AsyncWebServerRequest *req){ req->send(200,"text/plain","ok"); delay(150); resetAndReboot(); });
  _server->on("/wifi/restart",  HTTP_POST, [    ](AsyncWebServerRequest *req){ req->send(200,"text/plain","ok"); delay(150); ESP.restart(); });
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
  _mountHandlers();
}

bool JouleNetClass::_tryConnect(const String &ssid, const String &pass, bool hidden) {
  _activeSsid = ssid;
  WiFi.mode(WIFI_STA);
  // Disable Wi-Fi modem-sleep + crank TX power to the radio max. On a weak
  // link (RSSI worse than -75 dBm) the default settings drop TCP ACKs and
  // AsyncTCP stalls partway through a chunked HTML response. The ~75mA
  // delta is fine on a mains-powered device; battery firmware should
  // revisit these.
  WiFi.setSleep(false);
#if defined(ESP32)
  WiFi.setTxPower(WIFI_POWER_19_5dBm);   // 19.5 dBm ≈ 89 mW, the hardware max
#endif
  if (_staticOn) WiFi.config(_ip, _gw, _mask, _dnsIp);
  WiFi.begin(ssid.c_str(), pass.c_str(), 0, NULL, true);
  uint32_t start = millis();
  while ((millis() - start) < _connectTimeoutMs) {
    if (WiFi.status() == WL_CONNECTED) {
      _setState(NetState::Connected);
      _disconnectAt = 0;
#if defined(ESP32)
      MDNS.end(); MDNS.begin(_mdnsName.c_str()); MDNS.addService("http","tcp",80);
#endif
      return true;
    }
    delay(80);
  }
  return false;
}

void JouleNetClass::autoConnect() {
  if (_saved.empty()) { startPortal(); return; }
  _setState(NetState::Connecting);
  for (auto &c : _saved) if (_tryConnect(c.ssid, c.pass, c.hidden)) return;
  _setState(NetState::Failed);
  startPortal();
}

bool JouleNetClass::blockingConnect(uint32_t timeoutMs) {
  autoConnect();
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

void JouleNetClass::startPortal() { _startSoftAp(); _portalStartedAt = millis(); _setState(NetState::Portal); }
void JouleNetClass::stopPortal()  { _dns.stop(); _dnsRunning = false; WiFi.softAPdisconnect(true); }

void JouleNetClass::resetAndReboot() {
#if defined(ESP32)
  gNvs.begin(NS, false); gNvs.clear(); gNvs.end();
#endif
  delay(200); ESP.restart();
}

void JouleNetClass::loop() {
  if (_dnsRunning) _dns.processNextRequest();

  // Portal timeout: drop AP after a while of nobody using it so the radio
  // returns to pure STA mode.
  if (_state == NetState::Portal && _portalTimeoutMs > 0 &&
      (millis() - _portalStartedAt) > _portalTimeoutMs && !_saved.empty()) {
    stopPortal(); autoConnect();
  }

  if (_state == NetState::Connected) {
    if (WiFi.status() != WL_CONNECTED) {
      _setState(NetState::Connecting);
      _disconnectAt = millis();
      if (_autoReconnect) WiFi.reconnect();
    }
  } else if (_state == NetState::Connecting && WiFi.status() == WL_CONNECTED) {
    _setState(NetState::Connected);
  }

  // Reprovision watchdog: if we've been disconnected for too long, pop the
  // portal so a tech in front of the device can re-onboard it.
  if (_disconnectAt && _reprovisionMs &&
      (millis() - _disconnectAt) > _reprovisionMs &&
      _state != NetState::Portal) {
    startPortal();
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
String    JouleNetClass::paramValue(const String &key) const {
  for (auto &p : _params) if (p.key == key) return p.value;
  return String();
}

} // namespace joule

joule::JouleNetClass JouleNet;
