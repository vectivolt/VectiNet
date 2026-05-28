// ---------------------------------------------------------------------------
// JouleSuite for ESP32 / ESP8266 — JouleOTA · JouleSerial · JouleNet · JouleDash
// Author: Chinmoy Bhuyan
// Email:  dikibhuyan@gmail.com
// (c) 2026 — MIT License
// ---------------------------------------------------------------------------
//
// CustomFields — every JouleNet custom-parameter type rendered on the
// Setup tab of the captive portal. Persisted to NVS automatically;
// `paramValue("key")` reads them anywhere in your sketch.

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <JouleNet.h>

AsyncWebServer server(80);

void setup() {
  Serial.begin(115200);

  JouleNet.setApCredentials("MyProduct-Setup");
  JouleNet.setHostname("myproduct");
  JouleNet.setTitle("MyProduct · Setup");
  JouleNet.setBrandColor("#0ea5e9");

  // ---- All nine custom-parameter types ----------------------------------
  using P = joule::NetParam;
  using T = joule::NetParamType;
  JouleNet.addParameter(P{"sec_app",  "Application",     T::Header,   "","","",0,0});
  JouleNet.addParameter(P{"name",     "Display name",    T::Text,     "Living-room widget","what to show in the title","",0,0});
  JouleNet.addParameter(P{"mqtt_h",   "MQTT host",       T::Text,     "broker.local","FQDN or IP","",0,0});
  JouleNet.addParameter(P{"mqtt_p",   "MQTT port",       T::Number,   "1883","","",1,65535});
  JouleNet.addParameter(P{"mqtt_user","MQTT username",   T::Text,     "","","",0,0});
  JouleNet.addParameter(P{"mqtt_pw",  "MQTT password",   T::Password, "","","",0,0});
  JouleNet.addParameter(P{"region",   "Region",          T::Dropdown, "EU","","EU|US|APAC|IN|other",0,0});
  JouleNet.addParameter(P{"colour",   "Accent colour",   T::Color,    "#0ea5e9","","",0,0});
  JouleNet.addParameter(P{"verbose",  "Verbose logs",    T::Toggle,   "1","","",0,0});
  JouleNet.addParameter(P{"sec_notes","Notes",           T::Divider,  "","","",0,0});
  JouleNet.addParameter(P{"notes",    "Installation notes", T::Textarea,
    "Customer: ACME Ltd\nInstall date: 2026-03-12\nTech: K. Patel","free-form","",0,0});

  JouleNet.onConfig([](const std::vector<joule::NetParam>&){
    Serial.println("settings saved — reconfiguring MQTT");
    // re-init your downstream clients here
  });

  JouleNet.begin(&server);
  server.begin();
  JouleNet.autoConnect();
  Serial.println("portal at http://" + WiFi.localIP().toString() + "/wifi");
}

void loop() {
  JouleNet.loop();
}
