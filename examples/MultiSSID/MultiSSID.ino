// ---------------------------------------------------------------------------
// JouleSuite for ESP32 / ESP8266 — JouleOTA · JouleSerial · JouleNet · JouleDash
// Author: Chinmoy Bhuyan
// Email:  dikibhuyan@gmail.com
// (c) 2026 — MIT License
// ---------------------------------------------------------------------------
//
// MultiSSID — save several networks; JouleNet tries them in saved order
// on every boot and switches automatically if the active one drops.
// Great for devices that get carried between home, office, lab, and the
// guest network at a customer site.

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <JouleNet.h>

AsyncWebServer server(80);

void setup() {
  Serial.begin(115200);

  // Seed up to 8 networks. Hidden SSIDs work too — set the third arg true.
  JouleNet.saveCredentials("HomeWifi",      "homepass");
  JouleNet.saveCredentials("Office-5G",     "officepass");
  JouleNet.saveCredentials("Lab-Mesh",      "labpass");
  JouleNet.saveCredentials("CustomerGuest", "guestpass");
  // Hidden network:
  JouleNet.saveCredentials("Secret-AP", "hiddenpass", /*hidden=*/true);

  JouleNet.setApCredentials("Widget-Setup");
  JouleNet.setHostname("widget");
  JouleNet.setReprovisionMs(120000);   // pop portal if offline > 2 min

  JouleNet.onState([](joule::NetState s){
    const char *names[] = {"idle","connecting","connected","portal","failed"};
    Serial.printf("state → %s   ssid=%s\n", names[(int)s], JouleNet.activeSsid().c_str());
  });

  JouleNet.begin(&server);
  server.begin();
  JouleNet.autoConnect();   // tries the saved list in order
}

void loop() {
  JouleNet.loop();
}
