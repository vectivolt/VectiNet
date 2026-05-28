// ---------------------------------------------------------------------------
// JouleSuite for ESP32 / ESP8266 — JouleOTA · JouleSerial · JouleNet · JouleDash
// Author: Chinmoy Bhuyan
// Email:  dikibhuyan@gmail.com
// (c) 2026 — MIT License
// ---------------------------------------------------------------------------

// JouleNet — Wi-Fi provisioning and network manager.
//
// Why this exists: NetWizard.pro is closed-source, supports a single SSID,
// and doesn't expose post-setup diagnostics. JouleNet adds:
//
//   * Multi-SSID list with automatic failover — every saved network is
//     ranked by signal strength and tried in order on boot.
//   * Captive portal that pops up the setup page on every connected device
//     (DNS hijack on the SoftAP).
//   * Custom parameters (text / password / number / toggle / dropdown /
//     color) merged into the setup form — perfect for one-shot device
//     onboarding (MQTT host, room name, calibration constants, …).
//   * Static-IP, hostname, mDNS, country-code and hidden-SSID configuration.
//   * /wifi/status JSON for in-place diagnostics: signal, BSSID, AP MAC,
//     gateway, DNS, uptime, last disconnect reason.
//   * NVS-backed persistence with atomic replace (writes to a temp key
//     first, then swaps) so a power-cut mid-write can't corrupt config.
//   * Sync + async modes — block waiting for connection during setup(),
//     or fire-and-forget with a callback.
//   * Auto-reprovision: if connection stays down longer than
//     `setReprovisionMs()`, the portal comes back up automatically.

#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <functional>
#include <vector>

#if defined(ESP32)
  #include <WiFi.h>
  #include <DNSServer.h>
  #include <ESPmDNS.h>
  #include <Preferences.h>
#elif defined(ESP8266)
  #include <ESP8266WiFi.h>
  #include <DNSServer.h>
  #include <ESP8266mDNS.h>
  #include <EEPROM.h>
#endif

namespace joule {

enum class NetState : uint8_t {
  Idle = 0,
  Connecting,
  Connected,
  Portal,
  Failed,
};

enum class NetParamType : uint8_t {
  Text=0, Password, Number, Toggle, Dropdown, Color, Header, Divider, Textarea
};

struct NetParam {
  String       key;          // NVS key & form field name
  String       label;        // shown to user
  NetParamType type;
  String       value;        // current value (string-encoded)
  String       hint;         // placeholder / help text
  String       opts;         // dropdown options: "one|two|three"
  int          min = 0, max = 0;   // for Number
};

struct NetCreds { String ssid; String pass; bool hidden=false; };

using NetStateCb = std::function<void(NetState)>;
using NetCfgCb   = std::function<void(const std::vector<NetParam>&)>;

class JouleNetClass {
public:
  JouleNetClass();

  // Identify the AP shown to users when the portal is up. The AP password
  // is optional but recommended in public spaces. autoConnect() is async-
  // friendly: returns immediately, status accessible via getState().
  void setApCredentials(const String &ssid, const String &password = "");
  void setHostname(const String &h)        { _hostname = h; }
  void setPortalTimeoutMs(uint32_t ms)     { _portalTimeoutMs = ms; }
  void setConnectTimeoutMs(uint32_t ms)    { _connectTimeoutMs = ms; }
  void setReprovisionMs(uint32_t ms)       { _reprovisionMs = ms; }
  void setStaticIP(IPAddress ip, IPAddress gw, IPAddress mask, IPAddress dns = (uint32_t)0);
  void clearStaticIP();
  void setCountryCode(const String &cc)    { _countryCode = cc; }
  void setMdnsName(const String &n)        { _mdnsName = n; }
  void setBrandColor(const String &css)    { _brandColor = css; }
  void setTitle(const String &t)           { _title = t; }
  void setAutoReconnect(bool on)           { _autoReconnect = on; }

  // Save/clear saved networks. saveCredentials() appends; the next call to
  // _connectBest() will try the strongest one first.
  void saveCredentials(const String &ssid, const String &password, bool hidden=false);
  void clearAllCredentials();
  std::vector<NetCreds> savedNetworks() const { return _saved; }

  // Custom parameters appended to the portal form. Persistence is automatic
  // via NVS (key == NetParam.key). Call addParameter() BEFORE begin().
  void addParameter(const NetParam &p);

  // Entry points.
  void begin(AsyncWebServer *server);                 // mount /wifi endpoints
  void autoConnect();                                 // try saved → portal if all fail
  bool blockingConnect(uint32_t timeoutMs = 20000);   // sync convenience
  void startPortal();                                 // force portal up
  void stopPortal();
  void resetAndReboot();                              // wipe NVS, reboot

  // Callbacks.
  void onState(NetStateCb cb)   { _onState = std::move(cb); }
  void onConfig(NetCfgCb cb)    { _onConfig = std::move(cb); }

  // Loop hook — services DNS, portal timeout, reprovision watchdog.
  void loop();

  // Status accessors.
  NetState  getState()    const { return _state; }
  IPAddress localIP()     const;
  IPAddress gatewayIP()   const;
  IPAddress subnetMask()  const;
  IPAddress apIP()        const;
  String    bssid()       const;
  int       rssi()        const;
  uint8_t   channel()     const;
  String    activeSsid()  const { return _activeSsid; }
  String    paramValue(const String &key) const;

private:
  void _setState(NetState s);
  void _loadFromNvs();
  void _saveToNvs();
  void _connectBest();
  bool _tryConnect(const String &ssid, const String &pass, bool hidden);
  void _startSoftAp();
  void _mountHandlers();
  String _renderPortal() const;
  String _statusJson() const;

  AsyncWebServer *_server = nullptr;
  DNSServer       _dns;
  bool            _dnsRunning = false;

  String   _apSsid     = "Joule-Setup";
  String   _apPass;
  String   _hostname   = "joule";
  String   _mdnsName   = "joule";
  String   _countryCode= "01";
  String   _brandColor = "#3da9fc";
  String   _title      = "JouleNet · Setup";
  bool     _autoReconnect = true;

  uint32_t _portalTimeoutMs  = 180000;
  uint32_t _connectTimeoutMs = 15000;
  uint32_t _reprovisionMs    = 60000;
  uint32_t _portalStartedAt  = 0;
  uint32_t _disconnectAt     = 0;

  bool     _staticOn = false;
  IPAddress _ip, _gw, _mask, _dnsIp;

  std::vector<NetCreds> _saved;
  std::vector<NetParam> _params;

  NetState  _state = NetState::Idle;
  String    _activeSsid;

  NetStateCb _onState;
  NetCfgCb   _onConfig;
};

} // namespace joule

extern joule::JouleNetClass JouleNet;
