// ---------------------------------------------------------------------------
// VectiSuite for ESP32 / ESP8266 — VectiOTA · VectiSerial · VectiNet · VectiDash
// Author: VectiVolt team
// (c) 2026 VectiVolt — Apache-2.0 License
// ---------------------------------------------------------------------------
//
// CustomFields — every VectiNet custom-parameter type rendered on the
// Setup tab of the captive portal. Persisted to NVS automatically;
// `paramValue("key")` reads them anywhere in your sketch.

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <VectiNet.h>

AsyncWebServer server(80);

void setup() {
  Serial.begin(115200);

  VectiNet.setApCredentials("MyProduct-Setup");
  VectiNet.setHostname("myproduct");
  VectiNet.setTitle("MyProduct · Setup");
  VectiNet.setBrandColor("#0ea5e9");

  // ---- All nine custom-parameter types ----------------------------------
  using P = vecti::NetParam;
  using T = vecti::NetParamType;
  VectiNet.addParameter(P{"sec_app",  "Application",     T::Header,   "","","",0,0});
  VectiNet.addParameter(P{"name",     "Display name",    T::Text,     "Living-room widget","what to show in the title","",0,0});
  VectiNet.addParameter(P{"mqtt_h",   "MQTT host",       T::Text,     "broker.local","FQDN or IP","",0,0});
  VectiNet.addParameter(P{"mqtt_p",   "MQTT port",       T::Number,   "1883","","",1,65535});
  VectiNet.addParameter(P{"mqtt_user","MQTT username",   T::Text,     "","","",0,0});
  VectiNet.addParameter(P{"mqtt_pw",  "MQTT password",   T::Password, "","","",0,0});
  VectiNet.addParameter(P{"region",   "Region",          T::Dropdown, "EU","","EU|US|APAC|IN|other",0,0});
  VectiNet.addParameter(P{"colour",   "Accent colour",   T::Color,    "#0ea5e9","","",0,0});
  VectiNet.addParameter(P{"verbose",  "Verbose logs",    T::Toggle,   "1","","",0,0});
  VectiNet.addParameter(P{"sec_notes","Notes",           T::Divider,  "","","",0,0});
  VectiNet.addParameter(P{"notes",    "Installation notes", T::Textarea,
    "Customer: ACME Ltd\nInstall date: 2026-03-12\nTech: K. Patel","free-form","",0,0});

  VectiNet.onConfig([](const std::vector<vecti::NetParam>&){
    Serial.println("settings saved — reconfiguring MQTT");
    // re-init your downstream clients here
  });

  // autoConnect() returns before the radio has associated, so there is no
  // IP to print here — wait for the state change.
  VectiNet.onState([](vecti::NetState s){
    if (s == vecti::NetState::Connected)
      Serial.println("portal at http://" + WiFi.localIP().toString() + "/wifi");
    else if (s == vecti::NetState::Portal)
      Serial.println("portal at http://" + WiFi.softAPIP().toString() + "/wifi");
  });

  VectiNet.begin(&server);
  server.begin();
  VectiNet.autoConnect();
}

void loop() {
  VectiNet.loop();
}
