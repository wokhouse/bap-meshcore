#include "GatewayTask.h"
#include "MyMesh.h"
#include "ArrivalDisplay.h"
#include "SiriParser.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>

static const char* NTP_SERVER = "pool.ntp.org";
static const long  GMT_OFFSET_SEC = 0;
static const long  DAYLIGHT_OFFSET_SEC = 0;
// Earliest plausible "NTP has synced" timestamp. 2024-01-01 UTC. time(nullptr)
// returns 0 until SNTP resolves, so anything below this means "not synced yet".
static const time_t NTP_SANITY_EPOCH = 1704067200;

void GatewayTask::begin() {
  // Load private key
  if (BapConfig::loadPrivKey(_privkey)) {
    _has_privkey = true;
  }

  // Connect WiFi
  if (_cfg->wifi_ssid[0] && _cfg->wifi_pass[0]) {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin(_cfg->wifi_ssid, _cfg->wifi_pass);
    _wifi_started = true;
    Serial.println("WiFi: connecting...");
  } else {
    Serial.println("WiFi: no credentials configured");
  }
}

void GatewayTask::loop() {
  unsigned long now = millis();

  // Periodic WiFi status check
  if (_wifi_started && (now - _last_wifi_check > 5000)) {
    _last_wifi_check = now;
    bool connected = (WiFi.status() == WL_CONNECTED);
    if (connected != _wifi_connected) {
      _wifi_connected = connected;
      if (connected) {
        Serial.print("WiFi: connected, IP ");
        Serial.println(WiFi.localIP());
        // Configure NTP once we have network
        configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
      } else {
        Serial.println("WiFi: disconnected");
        // Force re-validation of NTP on reconnect — a long outage may have let
        // the RTC drift, and we don't want to broadcast bogus ETAs.
        _ntp_synced = false;
        if (_display) _display->setStatus("WiFi DOWN");
      }
    }
  }

  if (!_wifi_connected) return;
  if (!_has_privkey) {
    if (_display) _display->setStatus("NO PRIVKEY");
    return;
  }
  if (!_cfg->has_pubkey) {
    if (_display) _display->setStatus("NO PUBKEY");
    return;
  }
  if (_cfg->num_stops == 0) {
    if (_display) _display->setStatus("NO STOPS");
    return;
  }

  // Gate the first poll on NTP being actually synced. configTime() is async —
  // it returns immediately and time(nullptr) reads 0 (or garbage) for several
  // seconds. Polling before sync produced ~245-minute ETA offsets because
  // (arrive_sec - ~0)/60 is huge and clamps to the 254 cap. The second poll
  // looked correct only because NTP had synced by then.
  if (!_ntp_synced) {
    time_t t = time(nullptr);
    if (t < NTP_SANITY_EPOCH) {
      if (_display) _display->setStatus("NTP SYNC...");
      return;
    }
    _ntp_synced = true;
    Serial.printf("NTP: synced (%lu)\n", (unsigned long)t);
    // First poll within 2s of sync, then on the normal interval.
    _next_poll_millis = now + 2000UL;
  }

  // Poll
  if ((long)(now - _next_poll_millis) >= 0) {
    _next_poll_millis = now + (unsigned long)_cfg->poll_interval_sec * 1000UL;
    for (uint8_t i = 0; i < _cfg->num_stops; i++) {
      pollStop_(_cfg->stops[i]);
    }
  }
}

void GatewayTask::pollStop_(uint16_t stop_code) {
  String url = String(_cfg->endpoint);
  // Build URL — bap-http expects GET /stopmonitoring?stopcode=<code>
  if (!url.endsWith("/")) url += '/';
  url += "stopmonitoring?stopcode=";
  url += String(stop_code);

  String body;
  if (!httpGet_(url, body)) {
    Serial.printf("HTTP: GET failed for stop %u\n", stop_code);
    if (_display) _display->setStatus("HTTP FAIL");
    return;
  }

  bool response_ok = false;
  ArrivalVisit visits[BAP_MAX_VISITS];
  uint8_t n = SiriParser::parse(body, stop_code, visits, &response_ok);

  if (!response_ok) {
    Serial.printf("SIRI: stale/error for stop %u\n", stop_code);
    if (_display) _display->setStatus("HTTP 503");
    return;
  }

  Serial.printf("SIRI: stop %u got %u arrivals\n", stop_code, n);

  // Update local display (gateway shows the first configured stop)
  if (_display && stop_code == _cfg->stops[0] && n > 0) {
    _display->updateArrivals(stop_code, visits, n);
  }

  // Broadcast over mesh
  if (n > 0) {
    sendOverMesh_(stop_code, visits, n);
  }
}

bool GatewayTask::httpGet_(const String& url, String& out) {
  HTTPClient http;
  if (!http.begin(url)) return false;

  // Bearer auth
  String auth = "Bearer ";
  auth += _cfg->secret;
  http.setAuthorization("");  // clear any basic auth
  http.addHeader("Authorization", auth);

  int code = http.GET();
  if (code != 200) {
    Serial.printf("HTTP: %d for %s\n", code, url.c_str());
    http.end();
    return false;
  }

  out = http.getString();
  http.end();
  return true;
}

void GatewayTask::sendOverMesh_(uint16_t stop_code, const ArrivalVisit* visits, uint8_t n) {
  size_t data_len = BapPacket::encode(_tx_buf, stop_code, visits, n);
  if (data_len == 0) {
    Serial.println("BAP: encode failed (overflow)");
    return;
  }
  size_t total = BapPacket::sign(_tx_buf, data_len, _privkey);
  if (total == 0) {
    Serial.println("BAP: sign failed");
    return;
  }
  // _mesh->sendBapBroadcast wraps the data in group-datagram encryption + floods.
  // It logs the specific failure (oversize vs pool-exhausted); we just report the outcome.
  if (!_mesh->sendBapBroadcast(_tx_buf, total)) {
    Serial.printf("BAP: broadcast stop %u NOT sent\n", stop_code);
  } else {
    Serial.printf("BAP: broadcast stop %u (%u bytes)\n", stop_code, (unsigned)total);
  }
}
