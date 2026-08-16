// ---------------------------------------------------------------------------
// VectiSuite for ESP32 / ESP8266 — VectiOTA · VectiSerial · VectiNet · VectiDash
// Author: VectiVolt team
// (c) 2026 VectiVolt — Apache-2.0 License
// ---------------------------------------------------------------------------

// VectiNet — Wi-Fi provisioning and network manager.
//
// Why this exists: NetWizard.pro is closed-source, supports a single SSID,
// and doesn't expose post-setup diagnostics. VectiNet adds:
//
//   * Multi-SSID list with automatic failover — up to 8 saved networks,
//     tried in saved order until one associates. The sweep is driven from
//     loop(), one attempt at a time, so the sketch keeps running.
//   * Captive portal that pops up the setup page on every connected device
//     (DNS hijack on the SoftAP).
//   * Custom parameters (text / password / number / toggle / dropdown /
//     color / textarea / read-only display) merged into the setup form —
//     perfect for one-shot device onboarding (MQTT host, room name,
//     calibration constants, device fingerprint, …).
//   * Portal UX shaping (setUiDefaultTab / setUiHideWifiTab) so an AP-only
//     setup appliance can open straight on its parameter form.
//   * Static-IP, hostname, mDNS, country-code and hidden-SSID configuration.
//   * /wifi/status JSON for in-place diagnostics: signal, BSSID, AP MAC,
//     gateway, DNS, uptime, last disconnect reason.
//   * NVS-backed persistence (Preferences). Writes are not transactional:
//     the network count is written last, so a power cut mid-write can lose
//     the newest slot but can never make NVS claim a slot it doesn't hold.
//   * Sync + async modes — block waiting for connection during setup(),
//     or fire-and-forget with a callback.
//   * Auto-reprovision: if connection stays down longer than
//     `setReprovisionMs()`, the portal comes back up automatically.
//
// ESP32 only. Persistence (Preferences/NVS), mDNS and the regulatory
// country code all sit on ESP-IDF APIs; the ESP8266 equivalents are a
// different enough shape that a half-ported build would silently lose every
// credential on reboot instead of failing loudly, so it fails loudly here.

#pragma once
#include <Arduino.h>
#include <ESPAsyncWebServer.h>
#include <functional>
#include <vector>

#if !defined(ESP32)
  #error "VectiNet supports ESP32 only (needs Preferences/NVS, ESPmDNS and esp_wifi)."
#endif

#include <WiFi.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>

namespace vecti {

enum class NetState : uint8_t {
  Idle = 0,
  Connecting,
  Connected,
  Portal,
  Failed,
};

enum class NetParamType : uint8_t {
  Text=0, Password, Number, Toggle, Dropdown, Color, Header, Divider, Textarea,
  // Read-only value field. Renders the firmware-supplied `value` in a
  // monospace box with a copy-to-clipboard button — for things the operator
  // must read/copy but never edit (device fingerprint, serial, build id).
  // Its content is firmware-owned: never persisted to NVS, never round-
  // tripped on save (see isInputParam in the .cpp).
  Display
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

// `hidden` is metadata only: esp_wifi associates by configured SSID and
// already sends a directed probe for it, so there is no separate lever to
// pull. It is kept because it round-trips through the portal JSON and NVS,
// and because a UI showing "this one is hidden" is worth the byte.
struct NetCreds { String ssid; String pass; bool hidden=false; };

using NetStateCb = std::function<void(NetState)>;
using NetCfgCb   = std::function<void(const std::vector<NetParam>&)>;

class VectiNetClass {
public:
  VectiNetClass();

  // Identify the AP shown to users when the portal is up. The AP password
  // is optional but recommended in public spaces. autoConnect() is async-
  // friendly: returns immediately, status accessible via getState().
  void setApCredentials(const String &ssid, const String &password = "");

  // Identity setters. A value set here is the device's identity and wins
  // over whatever is in NVS — otherwise a stale persisted hostname (written
  // the first time anyone saved a network) would outlive every reflash and
  // setHostname() would be a permanent no-op. The flip side: on a sketch
  // that hardcodes these, the portal's matching field only lasts until the
  // next reboot. Leave them unset to let the portal own them.
  void setHostname(const String &h)        { _hostname = h; _hostnameFromCode = true; }
  void setCountryCode(const String &cc)    { _countryCode = cc; _countryFromCode = true; }
  void setMdnsName(const String &n)        { _mdnsName = n; _mdnsFromCode = true; }

  void setPortalTimeoutMs(uint32_t ms)     { _portalTimeoutMs = ms; }
  void setConnectTimeoutMs(uint32_t ms)    { _connectTimeoutMs = ms; }
  void setReprovisionMs(uint32_t ms)       { _reprovisionMs = ms; }
  void setStaticIP(IPAddress ip, IPAddress gw, IPAddress mask, IPAddress dns = (uint32_t)0);
  void clearStaticIP();
  void setBrandColor(const String &css)    { _brandColor = css; }
  void setTitle(const String &t)           { _title = t; }
  void setAutoReconnect(bool on)           { _autoReconnect = on; }

  // Portal UX shaping — which tab opens first, and whether the Wi-Fi
  // scan/join tab is shown at all. For an AP-only "setup appliance" that
  // never joins a station network (e.g. a device whose only network role
  // is to host this config portal), open straight on the parameter form
  // and hide the Wi-Fi tab so the operator isn't faced with an irrelevant
  // network picker. Default keeps the classic provisioning UX.
  void setUiDefaultTab(const String &tab)  { _uiDefaultTab = tab; }   // "wifi" | "params" | "status"
  void setUiHideWifiTab(bool hide)         { _uiHideWifi = hide; }

  // HTTP Basic auth for EVERY /wifi route, including the portal HTML and the
  // read-only JSON: /wifi/status and /wifi/scan hand out the device MAC, the
  // BSSIDs around it and the SSIDs it can see, which is site topology, not
  // public information. The HTML is gated too because a 401 answering a
  // fetch() never prompts — the browser only pops the credential dialog on a
  // top-level navigation, and then reuses those credentials for the SPA's
  // own requests. Empty user disables the gate (back-compat, and what the
  // examples run with). Call before begin().
  void setAuth(const String &user, const String &pass) { _authUser = user; _authPass = pass; }

  // Save/clear saved networks. saveCredentials() updates an existing SSID in
  // place, otherwise appends. Returns false when the 8 persisted slots are
  // full — accepting a 9th would work until the next reboot and then vanish.
  bool saveCredentials(const String &ssid, const String &password, bool hidden=false);
  void clearAllCredentials();
  std::vector<NetCreds> savedNetworks() const { return _saved; }

  // Custom parameters appended to the portal form. Persistence is automatic
  // via NVS (key == NetParam.key). Call addParameter() BEFORE begin().
  void addParameter(const NetParam &p);

  // Entry points.
  void begin(AsyncWebServer *server);                 // mount /wifi endpoints
  // Arms the connect sweep and returns immediately; loop() drives it and
  // raises the portal if every saved network fails.
  void autoConnect();
  // Same sweep, but spins loop() until it settles or timeoutMs elapses —
  // timeoutMs is the whole budget, not per network. Returns false on
  // timeout with the sweep still running in the background.
  bool blockingConnect(uint32_t timeoutMs = 20000);
  void startPortal();                                 // force portal up
  void stopPortal();
  void resetAndReboot();                              // wipe NVS, reboot

  // Callbacks. Both fire on the Arduino task (from setup() or loop()), never
  // on the AsyncTCP task, even when an HTTP request caused the change —
  // touching AsyncWebSocket or VectiSerial from the AsyncTCP task races
  // whatever the sketch is doing with them. onConfig() therefore arrives on
  // the loop() tick after the operator hit Save, with a snapshot of the
  // parameter list rather than the live one.
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
  // Copied under the same mutex the portal handlers take, so it is safe from
  // a sketch's own AsyncWebServer handler as well as from loop() — the connect
  // sweep reassigns the underlying String once per attempt. Yours to keep.
  String    activeSsid()  const;
  // Safe to call from loop() while the operator is saving the Setup tab: the
  // value is copied under the same mutex the portal handler takes (the POST
  // runs on the AsyncTCP task and reassigns these Strings). Yours to keep.
  String    paramValue(const String &key) const;

private:
  void _setState(NetState s);
  void _loadFromNvs();
  void _saveToNvs();
  void _startSweep(size_t firstIdx);            // arm the loop()-driven sweep
  void _pumpConnect();                          // one tick of that sweep
  void _beginAttempt(const NetCreds &c);        // non-blocking WiFi.begin()
  void _onLinkUp();                             // association succeeded
  void _applyPendingConnect();                  // portal request → this task
  void _applyCountry();                         // _countryCode → esp_wifi
  void _startSoftAp();
  void _mountHandlers();
  bool _authGate(AsyncWebServerRequest *req);   // true if allowed (or auth off)
  String _statusJson() const;

  AsyncWebServer *_server = nullptr;
  DNSServer       _dns;
  bool            _dnsRunning = false;

  String   _apSsid     = "Vecti-Setup";
  String   _apPass;
  String   _hostname   = "vecti";
  String   _mdnsName   = "vecti";
  String   _countryCode= "01";
  String   _brandColor = "#3da9fc";
  String   _title      = "VectiNet · Setup";
  bool     _autoReconnect = true;
  String   _uiDefaultTab = "wifi";   // tab the SPA opens on (see setUiDefaultTab)
  bool     _uiHideWifi   = false;    // hide the Wi-Fi scan/join tab entirely

  uint32_t _portalTimeoutMs  = 180000;
  uint32_t _connectTimeoutMs = 15000;
  uint32_t _reprovisionMs    = 60000;
  uint32_t _portalStartedAt  = 0;
  // Timestamps are stored with the low bit forced on, so 0 stays an
  // unambiguous "not armed" even in the millis() rollover tick.
  uint32_t _disconnectAt     = 0;
  uint32_t _lastScanAt       = 0;
  uint32_t _rebootAt         = 0;   // deferred so the HTTP response can flush
  bool     _rebootWipesNvs   = false;
  // The library raised this portal (reprovision watchdog / exhausted sweep),
  // not the sketch, so loop() may take it down again by itself if the link
  // comes back. Kept separate from _disconnectAt, which startPortal() disarms
  // so the watchdog cannot re-raise the AP on the very next loop() pass.
  bool     _portalAuto = false;
  bool     _nvsLoaded          = false;   // _loadFromNvs() has run at least once

  // Connect sweep state (advanced only from loop(), see _pumpConnect).
  bool     _sweeping         = false;
  size_t   _sweepIdx         = 0;
  uint32_t _attemptStartedAt = 0;   // 0 = no association in flight

  // A portal POST hands its work to loop() instead of doing it inline: the
  // handler runs on the AsyncTCP task, and saving credentials there would
  // reallocate _saved and write flash underneath the sweep. Every string is
  // stored before _pcPending, which is the only field loop() tests. One slot,
  // one writer: while _pcPending is set the handler answers 409 rather than
  // overwriting Strings that _applyPendingConnect() may be persisting.
  volatile bool _pcPending = false;
  // Set by the /wifi/params POST (AsyncTCP task) after it has updated the
  // values; loop() does the flash write and the onConfig() callback.
  volatile bool _paramsDirty = false;
  String    _pcSsid, _pcPass, _pcHost, _pcCc;
  bool      _pcHidden = false;
  int8_t    _pcStatic = -1;         // -1 leave alone, 0 clear, 1 apply below
  IPAddress _pcIp, _pcGw, _pcMask, _pcDns;

  bool     _staticOn = false;
  IPAddress _ip, _gw, _mask, _dnsIp;
  bool     _hostnameFromCode = false, _countryFromCode = false;
  bool     _mdnsFromCode     = false, _staticFromCode  = false;

  String   _authUser;   // empty = mutating endpoints unauthenticated (back-compat)
  String   _authPass;

  std::vector<NetCreds> _saved;
  std::vector<NetParam> _params;

  NetState  _state = NetState::Idle;
  String    _activeSsid;

  NetStateCb _onState;
  NetCfgCb   _onConfig;
};

} // namespace vecti

extern vecti::VectiNetClass VectiNet;
