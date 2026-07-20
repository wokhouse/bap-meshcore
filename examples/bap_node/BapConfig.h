#pragma once

#include <Arduino.h>
#include <stdint.h>

#define BAP_SSID_MAX       33
#define BAP_PASS_MAX       64
#define BAP_ENDPOINT_MAX   96
#define BAP_SECRET_MAX     64
#define BAP_MAX_STOPS      8
#define BAP_PUBKEY_LEN     32
#define BAP_PRIVKEY_LEN    64
#define BAP_CONFIG_VERSION 1

enum class BapRole : uint8_t { CLIENT = 0, GATEWAY = 1 };

struct BapConfig {
  BapRole role = BapRole::CLIENT;
  char wifi_ssid[BAP_SSID_MAX] = {0};
  char wifi_pass[BAP_PASS_MAX] = {0};
  char endpoint[BAP_ENDPOINT_MAX] = {0};
  char secret[BAP_SECRET_MAX] = {0};          // bap-http bearer token

  uint16_t stops[BAP_MAX_STOPS] = {0};        // gateway: stops to broadcast
  uint8_t  num_stops = 0;
  uint16_t client_stop = 0;                   // client: single stop to display

  uint8_t  pubkey[BAP_PUBKEY_LEN] = {0};      // shared public key (both roles)
  bool     has_pubkey = false;

  uint32_t poll_interval_sec = 60;

  // Load /bap.cfg into this struct. Returns true on success.
  bool load();

  // Save this struct to /bap.cfg as JSON.
  bool save();

  // Load /bap.key binary file into privkey_out. Returns true on success.
  static bool loadPrivKey(uint8_t privkey_out[BAP_PRIVKEY_LEN]);

  // Save privkey to /bap.key (binary, 64 bytes).
  static bool savePrivKey(const uint8_t privkey[BAP_PRIVKEY_LEN]);

  // True if /bap.key exists.
  static bool hasPrivKey();

  // Render config as a human-readable string, masking sensitive fields.
  String maskedShow() const;

  // Hex helpers for serial CLI
  static bool parseHex(uint8_t* out, size_t out_len, const char* hex);
  static String toHex(const uint8_t* in, size_t in_len);
};
