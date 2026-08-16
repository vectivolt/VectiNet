// ---------------------------------------------------------------------------
// VectiSuite for ESP32 / ESP8266 — VectiOTA · VectiSerial · VectiNet · VectiDash
// Author: VectiVolt team
// (c) 2026 VectiVolt — Apache-2.0 License
// ---------------------------------------------------------------------------
//
// MultiSSID — save several networks; VectiNet tries them in saved order on
// every boot, one at a time, until one associates. Great for devices that
// get carried between home, office, lab, and the guest network at a
// customer site. Note the order is the saved order, not signal strength:
// put the network you expect most often first. If the active link drops the
// radio retries that same AP; the reprovision watchdog below is what
// eventually raises the portal.

#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <VectiNet.h>

AsyncWebServer server(80);

void setup() {
  Serial.begin(115200);

  // Seed up to 8 networks. Hidden SSIDs work too — set the third arg true.
  VectiNet.saveCredentials("HomeWifi",      "homepass");
  VectiNet.saveCredentials("Office-5G",     "officepass");
  VectiNet.saveCredentials("Lab-Mesh",      "labpass");
  VectiNet.saveCredentials("CustomerGuest", "guestpass");
  // Hidden network:
  VectiNet.saveCredentials("Secret-AP", "hiddenpass", /*hidden=*/true);

  VectiNet.setApCredentials("Widget-Setup");
  VectiNet.setHostname("widget");
  VectiNet.setReprovisionMs(120000);   // pop portal if offline > 2 min

  VectiNet.onState([](vecti::NetState s){
    const char *names[] = {"idle","connecting","connected","portal","failed"};
    Serial.printf("state → %s   ssid=%s\n", names[(int)s], VectiNet.activeSsid().c_str());
  });

  VectiNet.begin(&server);
  server.begin();
  VectiNet.autoConnect();   // tries the saved list in order
}

void loop() {
  VectiNet.loop();
}
