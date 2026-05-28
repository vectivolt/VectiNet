// ---------------------------------------------------------------------------
// JouleSuite for ESP32 / ESP8266 — JouleOTA · JouleSerial · JouleNet · JouleDash
// Author: Chinmoy Bhuyan
// Email:  dikibhuyan@gmail.com
// (c) 2026 — MIT License
// ---------------------------------------------------------------------------

// JouleNet basic provisioning example.
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <JouleNet.h>

AsyncWebServer server(80);

void setup() {
  Serial.begin(115200);
  JouleNet.setApCredentials("Joule-Setup","");
  JouleNet.setHostname("joule");
  JouleNet.setMdnsName("joule");
  JouleNet.addParameter({"mqtt_host","MQTT host", joule::NetParamType::Text, "broker.local","mqtt.example.com","",0,0});
  JouleNet.addParameter({"mqtt_port","MQTT port", joule::NetParamType::Number,"1883","","",1,65535});
  JouleNet.addParameter({"verbose", "Verbose logs", joule::NetParamType::Toggle, "0","","",0,0});
  JouleNet.begin(&server);
  server.begin();
  JouleNet.autoConnect();
}

void loop(){ JouleNet.loop(); }
