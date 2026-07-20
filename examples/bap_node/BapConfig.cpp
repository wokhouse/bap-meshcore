#include "BapConfig.h"
#include <SPIFFS.h>
#include <ArduinoJson.h>

static const char* CONFIG_PATH = "/bap.cfg";
static const char* PRIVKEY_PATH = "/bap.key";

static const char* roleToStr(BapRole r) {
  return (r == BapRole::GATEWAY) ? "gateway" : "client";
}

static BapRole strToRole(const char* s) {
  if (strcmp(s, "gateway") == 0) return BapRole::GATEWAY;
  return BapRole::CLIENT;
}

bool BapConfig::load() {
  File f = SPIFFS.open(CONFIG_PATH, "r");
  if (!f) return false;

  String json;
  json.reserve(f.size());
  while (f.available()) {
    String chunk = f.readString();
    json += chunk;
  }
  f.close();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) return false;

  uint32_t ver = doc["version"] | 0;
  if (ver != BAP_CONFIG_VERSION) return false;

  const char* role_str = doc["role"] | "client";
  role = strToRole(role_str);

  strncpy(wifi_ssid, doc["wifi_ssid"] | "", BAP_SSID_MAX - 1);
  strncpy(wifi_pass, doc["wifi_pass"] | "", BAP_PASS_MAX - 1);
  strncpy(endpoint, doc["endpoint"] | "", BAP_ENDPOINT_MAX - 1);
  strncpy(secret, doc["secret"] | "", BAP_SECRET_MAX - 1);

  JsonArray stops_arr = doc["stops"].as<JsonArray>();
  num_stops = 0;
  for (JsonVariant v : stops_arr) {
    if (num_stops >= BAP_MAX_STOPS) break;
    stops[num_stops++] = v.as<uint16_t>();
  }

  client_stop = doc["client_stop"] | 0;
  poll_interval_sec = doc["poll_interval_sec"] | 60;

  const char* pubkey_hex = doc["pubkey_hex"] | "";
  if (pubkey_hex[0] != '\0') {
    if (parseHex(pubkey, BAP_PUBKEY_LEN, pubkey_hex)) {
      has_pubkey = true;
    }
  }

  return true;
}

bool BapConfig::save() {
  JsonDocument doc;
  doc["version"] = BAP_CONFIG_VERSION;
  doc["role"] = roleToStr(role);
  doc["wifi_ssid"] = wifi_ssid;
  doc["wifi_pass"] = wifi_pass;
  doc["endpoint"] = endpoint;
  doc["secret"] = secret;

  JsonArray stops_arr = doc["stops"].to<JsonArray>();
  for (uint8_t i = 0; i < num_stops; i++) {
    stops_arr.add(stops[i]);
  }

  doc["client_stop"] = client_stop;
  doc["poll_interval_sec"] = poll_interval_sec;

  if (has_pubkey) {
    doc["pubkey_hex"] = toHex(pubkey, BAP_PUBKEY_LEN);
  } else {
    doc["pubkey_hex"] = "";
  }

  File f = SPIFFS.open(CONFIG_PATH, "w");
  if (!f) return false;

  serializeJson(doc, f);
  f.close();
  return true;
}

bool BapConfig::loadPrivKey(uint8_t privkey_out[BAP_PRIVKEY_LEN]) {
  File f = SPIFFS.open(PRIVKEY_PATH, "r");
  if (!f) return false;
  size_t read = f.readBytes((char*)privkey_out, BAP_PRIVKEY_LEN);
  f.close();
  return read == BAP_PRIVKEY_LEN;
}

bool BapConfig::savePrivKey(const uint8_t privkey[BAP_PRIVKEY_LEN]) {
  File f = SPIFFS.open(PRIVKEY_PATH, "w");
  if (!f) return false;
  size_t written = f.write(privkey, BAP_PRIVKEY_LEN);
  f.close();
  return written == BAP_PRIVKEY_LEN;
}

bool BapConfig::hasPrivKey() {
  return SPIFFS.exists(PRIVKEY_PATH);
}

String BapConfig::maskedShow() const {
  String s;
  s.reserve(256);
  s += "role:             "; s += roleToStr(role); s += "\n";
  s += "wifi_ssid:        "; s += wifi_ssid; s += "\n";
  s += "wifi_pass:        "; s += (wifi_pass[0] ? "***" : ""); s += "\n";
  s += "endpoint:         "; s += endpoint; s += "\n";
  s += "secret:           "; s += (secret[0] ? "***" : ""); s += "\n";
  s += "poll_interval:    "; s += (int)poll_interval_sec; s += "s\n";
  s += "stops (gateway):  ";
  for (uint8_t i = 0; i < num_stops; i++) {
    if (i) s += ",";
    s += stops[i];
  }
  s += "\n";
  s += "client_stop:      "; s += client_stop; s += "\n";
  s += "pubkey:           ";
  if (has_pubkey) s += toHex(pubkey, BAP_PUBKEY_LEN);
  else s += "(none)";
  s += "\n";
  s += "privkey:          ";
  s += hasPrivKey() ? "(present)" : "(none)";
  s += "\n";
  return s;
}

bool BapConfig::parseHex(uint8_t* out, size_t out_len, const char* hex) {
  size_t hex_len = strlen(hex);
  if (hex_len != out_len * 2) return false;
  for (size_t i = 0; i < out_len; i++) {
    char hi = hex[i * 2];
    char lo = hex[i * 2 + 1];
    auto parseNibble = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    int h = parseNibble(hi), l = parseNibble(lo);
    if (h < 0 || l < 0) return false;
    out[i] = (h << 4) | l;
  }
  return true;
}

String BapConfig::toHex(const uint8_t* in, size_t in_len) {
  static const char hexchars[] = "0123456789abcdef";
  String s;
  s.reserve(in_len * 2);
  for (size_t i = 0; i < in_len; i++) {
    s += hexchars[(in[i] >> 4) & 0xF];
    s += hexchars[in[i] & 0xF];
  }
  return s;
}
