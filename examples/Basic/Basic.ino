// ---------------------------------------------------------------------------
// VectiSuite for ESP32 / ESP8266 — VectiOTA · VectiSerial · VectiNet · VectiDash
// Author: Chinmoy Bhuyan
// Email:  chinmoy@joulepoint.com
// (c) 2026 VectiVolt — Apache-2.0 License
// ---------------------------------------------------------------------------

// VectiNet basic provisioning example.
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <VectiNet.h>

AsyncWebServer server(80);

void setup() {
  Serial.begin(115200);
  VectiNet.setApCredentials("Vecti-Setup","");
  VectiNet.setHostname("vecti");
  VectiNet.setMdnsName("vecti");
  VectiNet.addParameter({"mqtt_host","MQTT host", vecti::NetParamType::Text, "broker.local","mqtt.example.com","",0,0});
  VectiNet.addParameter({"mqtt_port","MQTT port", vecti::NetParamType::Number,"1883","","",1,65535});
  VectiNet.addParameter({"verbose", "Verbose logs", vecti::NetParamType::Toggle, "0","","",0,0});
  VectiNet.begin(&server);
  server.begin();
  VectiNet.autoConnect();
}

void loop(){ VectiNet.loop(); }
