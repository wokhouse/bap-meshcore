#pragma once

#include <Arduino.h>
#include "BapConfig.h"
#include "BapPacket.h"

class MyMesh;
class ArrivalDisplay;

class GatewayTask {
  BapConfig* _cfg;
  MyMesh* _mesh;
  ArrivalDisplay* _display;
  uint8_t _privkey[BAP_PRIVKEY_LEN];
  bool _has_privkey = false;
  unsigned long _next_poll_millis = 0;
  bool _wifi_started = false;
  bool _wifi_connected = false;
  bool _ntp_synced = false;
  unsigned long _last_wifi_check = 0;

  // Per-poll scratch buffer
  uint8_t _tx_buf[BAP_DATA_BUDGET + BAP_SIG_LEN];

public:
  GatewayTask(BapConfig& cfg, MyMesh& mesh, ArrivalDisplay& disp)
    : _cfg(&cfg), _mesh(&mesh), _display(&disp) {}

  // WiFi.begin + NTP sync. Loads private key from SPIFFS.
  void begin();

  // Non-blocking poll loop. Called from main loop().
  void loop();

  bool isWifiConnected() const { return _wifi_connected; }

private:
  void pollStop_(uint16_t stop_code);
  bool httpGet_(const String& url, String& out);
  void sendOverMesh_(uint16_t stop_code, const ArrivalVisit* visits, uint8_t n);
};
