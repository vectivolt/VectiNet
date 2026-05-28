// ---------------------------------------------------------------------------
// JouleSuite for ESP32 / ESP8266 — JouleOTA · JouleSerial · JouleNet · JouleDash
// Author: Chinmoy Bhuyan
// Email:  dikibhuyan@gmail.com
// (c) 2026 — MIT License
// ---------------------------------------------------------------------------
//
// StaticIP — fixed network configuration. Useful when DHCP is unreliable
// or when a colleague needs to find the device on a predictable address.

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <JouleNet.h>

AsyncWebServer server(80);

void setup() {
  Serial.begin(115200);

  // Pin the device to 192.168.1.200 with 8.8.8.8 as DNS.
  JouleNet.setStaticIP(
    IPAddress(192,168,1,200),
    IPAddress(192,168,1,1),
    IPAddress(255,255,255,0),
    IPAddress(8,8,8,8));

  JouleNet.setHostname("widget-200");
  JouleNet.setCountryCode("IN");
  JouleNet.saveCredentials("YOUR_SSID","YOUR_PASS");
  JouleNet.begin(&server);
  server.begin();
  JouleNet.blockingConnect(20000);

  Serial.printf("ip=%s\n", WiFi.localIP().toString().c_str());
}

void loop() { JouleNet.loop(); }
