// ---------------------------------------------------------------------------
// VectiSuite for ESP32 / ESP8266 — VectiOTA · VectiSerial · VectiNet · VectiDash
// Author: VectiVolt team
// (c) 2026 VectiVolt — Apache-2.0 License
// ---------------------------------------------------------------------------
//
// StaticIP — fixed network configuration. Useful when DHCP is unreliable
// or when a colleague needs to find the device on a predictable address.

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <VectiNet.h>

AsyncWebServer server(80);

void setup() {
  Serial.begin(115200);

  // Pin the device to 192.168.1.200 with 8.8.8.8 as DNS.
  VectiNet.setStaticIP(
    IPAddress(192,168,1,200),
    IPAddress(192,168,1,1),
    IPAddress(255,255,255,0),
    IPAddress(8,8,8,8));

  VectiNet.setHostname("widget-200");
  VectiNet.setCountryCode("IN");
  VectiNet.saveCredentials("YOUR_SSID","YOUR_PASS");
  VectiNet.begin(&server);
  server.begin();
  VectiNet.blockingConnect(20000);

  Serial.printf("ip=%s\n", WiFi.localIP().toString().c_str());
}

void loop() { VectiNet.loop(); }
