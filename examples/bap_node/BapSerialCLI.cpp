#include "BapSerialCLI.h"
#include "MyMesh.h"
#include <stdio.h>
#include <string.h>
#include <Identity.h>

void BapSerialCLI::loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') {
      // Process complete line
      Serial.println();
      _cmd_buf[_cmd_len] = '\0';
      if (_cmd_len > 0) dispatch_(_cmd_buf);
      _cmd_len = 0;
      prompt();
    } else if (c == '\n') {
      // Ignore LF (we wait for CR)
    } else if (_cmd_len < sizeof(_cmd_buf) - 1) {
      _cmd_buf[_cmd_len++] = c;
      Serial.print(c);   // local echo
    }
  }
}

void BapSerialCLI::prompt() {
  Serial.print("bap> ");
}

void BapSerialCLI::help() {
  Serial.println();
  Serial.println("BAP Node commands:");
  Serial.println("  role gateway|client          - set device role");
  Serial.println("  wifi SSID \"pass\"             - set WiFi credentials (gateway)");
  Serial.println("  endpoint \"http://host:port\"  - bap-http base URL");
  Serial.println("  secret \"token\"               - bap-http bearer secret");
  Serial.println("  keygen                       - (gateway) generate Ed25519 keypair");
  Serial.println("  pubkey <64-hex>              - (client) import gateway's pubkey");
  Serial.println("  stops add <code>             - (gateway) add stop to broadcast");
  Serial.println("  stops del <code>             - (gateway) remove stop");
  Serial.println("  stops list                   - list configured stops");
  Serial.println("  stop <code>                  - (client) set stop to display");
  Serial.println("  poll <sec>                   - poll interval (default 60)");
  Serial.println("  save                         - write config to SPIFFS");
  Serial.println("  show                         - show current config");
  Serial.println("  help                         - this message");
  Serial.println();
}

void BapSerialCLI::dispatch_(char* line) {
  char* saveptr;
  char* cmd = nextToken_(&saveptr);
  // Advance saveptr past leading skips done by nextToken_
  // Actually we passed &line initially — strtok_r-style; redo:
  // We'll re-tokenize using strtok_r on the same buffer.
  // (nextToken_ takes a saveptr and works on a string it modifies)
  // To keep it simple, use strtok_r directly here:
  char* sp = nullptr;
  char* tok = strtok_r(line, " \t", &sp);
  if (!tok) return;
  String cmd_s = String(tok);
  cmd_s.toLowerCase();
  const char* rest = sp ? sp : "";

  if (cmd_s == "help" || cmd_s == "?") {
    help();
  } else if (cmd_s == "role") {
    cmdRole_(rest);
  } else if (cmd_s == "wifi") {
    cmdWifi_(rest);
  } else if (cmd_s == "endpoint") {
    cmdEndpoint_(rest);
  } else if (cmd_s == "secret") {
    cmdSecret_(rest);
  } else if (cmd_s == "keygen") {
    cmdKeygen_(rest);
  } else if (cmd_s == "pubkey") {
    cmdPubkey_(rest);
  } else if (cmd_s == "stops") {
    char* sub = strtok_r(nullptr, " \t", &sp);
    if (!sub) { reply_("usage: stops add|del|list"); return; }
    String s = String(sub); s.toLowerCase();
    const char* arg = sp ? sp : "";
    if (s == "add") cmdStopsAdd_(arg);
    else if (s == "del") cmdStopsDel_(arg);
    else if (s == "list") cmdStopsList_();
    else reply_("unknown stops subcommand");
  } else if (cmd_s == "stop") {
    cmdStop_(rest);
  } else if (cmd_s == "poll") {
    cmdPoll_(rest);
  } else if (cmd_s == "save") {
    cmdSave_();
  } else if (cmd_s == "show") {
    cmdShow_();
  } else {
    reply_("unknown command (try 'help')");
  }
  (void)cmd;  // unused
}

void BapSerialCLI::cmdRole_(const char* arg) {
  // Take first whitespace-delimited token, lowercase compare
  String s = String(arg);
  s.trim();
  s.toLowerCase();
  if (s == "gateway") {
    _cfg->role = BapRole::GATEWAY;
    reply_("role: gateway (not yet saved)");
  } else if (s == "client") {
    _cfg->role = BapRole::CLIENT;
    reply_("role: client (not yet saved)");
  } else {
    reply_("usage: role gateway|client");
  }
}

void BapSerialCLI::cmdWifi_(const char* arg) {
  // wifi SSID "pass"
  char* sp = nullptr;
  char* ssid = strtok_r((char*)arg, " \t", &sp);
  if (!ssid) { reply_("usage: wifi SSID \"pass\""); return; }
  strncpy(_cfg->wifi_ssid, ssid, BAP_SSID_MAX - 1);
  _cfg->wifi_ssid[BAP_SSID_MAX - 1] = '\0';

  // Password: rest, possibly quoted
  char* pass = sp ? sp : (char*)"";
  // Strip surrounding quotes if present
  if (pass[0] == '"') {
    pass++;
    size_t len = strlen(pass);
    if (len > 0 && pass[len - 1] == '"') pass[len - 1] = '\0';
  }
  strncpy(_cfg->wifi_pass, pass, BAP_PASS_MAX - 1);
  _cfg->wifi_pass[BAP_PASS_MAX - 1] = '\0';
  reply_("wifi set (not yet saved)");
}

void BapSerialCLI::cmdEndpoint_(const char* arg) {
  String s = String(arg);
  s.trim();
  if (s.length() == 0) { reply_("usage: endpoint \"http://host:port\""); return; }
  // Strip quotes
  if (s.startsWith("\"") && s.endsWith("\"")) {
    s = s.substring(1, s.length() - 1);
  }
  strncpy(_cfg->endpoint, s.c_str(), BAP_ENDPOINT_MAX - 1);
  _cfg->endpoint[BAP_ENDPOINT_MAX - 1] = '\0';
  reply_("endpoint set (not yet saved)");
}

void BapSerialCLI::cmdSecret_(const char* arg) {
  String s = String(arg);
  s.trim();
  if (s.length() == 0) { reply_("usage: secret \"token\""); return; }
  if (s.startsWith("\"") && s.endsWith("\"")) {
    s = s.substring(1, s.length() - 1);
  }
  strncpy(_cfg->secret, s.c_str(), BAP_SECRET_MAX - 1);
  _cfg->secret[BAP_SECRET_MAX - 1] = '\0';
  reply_("secret set (not yet saved)");
}

void BapSerialCLI::cmdKeygen_(const char* arg) {
  (void)arg;
  // Generate a fresh Ed25519 keypair using mesh's RNG
  mesh::RNG* rng = _mesh->getRNG();
  if (!rng) { reply_("no RNG available"); return; }
  mesh::LocalIdentity id(rng);

  // Save private key to /bap.key
  uint8_t prv_buf[BAP_PRIVKEY_LEN];
  size_t written = id.writeTo(prv_buf, BAP_PRIVKEY_LEN);
  if (written < BAP_PRIVKEY_LEN) {
    // writeTo returns either PRV_KEY_SIZE (64) or PRV_KEY_SIZE+PUB_KEY_SIZE (96)
    // We just want the first 64 bytes
  }
  if (!BapConfig::savePrivKey(prv_buf)) {
    reply_("failed to save private key");
    return;
  }
  memcpy(_cfg->pubkey, id.pub_key, BAP_PUBKEY_LEN);
  _cfg->has_pubkey = true;

  String msg = "Public key: ";
  msg += BapConfig::toHex(_cfg->pubkey, BAP_PUBKEY_LEN);
  msg += "\n(paste this into clients with: pubkey <hex>)";
  reply_(msg);
}

void BapSerialCLI::cmdPubkey_(const char* arg) {
  String s = String(arg);
  s.trim();
  if (s.length() != BAP_PUBKEY_LEN * 2) {
    reply_("usage: pubkey <64 hex chars>");
    return;
  }
  if (!BapConfig::parseHex(_cfg->pubkey, BAP_PUBKEY_LEN, s.c_str())) {
    reply_("invalid hex");
    return;
  }
  _cfg->has_pubkey = true;
  reply_("pubkey set (not yet saved)");
}

void BapSerialCLI::cmdStopsAdd_(const char* arg) {
  if (_cfg->num_stops >= BAP_MAX_STOPS) { reply_("stops full"); return; }
  int code = atoi(arg);
  if (code <= 0 || code > 65535) { reply_("invalid stop code"); return; }
  // Avoid duplicates
  for (uint8_t i = 0; i < _cfg->num_stops; i++) {
    if (_cfg->stops[i] == (uint16_t)code) { reply_("already present"); return; }
  }
  _cfg->stops[_cfg->num_stops++] = (uint16_t)code;
  String msg = "added stop " + String(code);
  reply_(msg);
}

void BapSerialCLI::cmdStopsDel_(const char* arg) {
  int code = atoi(arg);
  if (code <= 0) { reply_("invalid stop code"); return; }
  bool found = false;
  for (uint8_t i = 0; i < _cfg->num_stops; i++) {
    if (found) {
      _cfg->stops[i - 1] = _cfg->stops[i];
    } else if (_cfg->stops[i] == (uint16_t)code) {
      found = true;
    }
  }
  if (found) {
    _cfg->num_stops--;
    String msg = "removed stop " + String(code);
    reply_(msg);
  } else {
    reply_("stop not found");
  }
}

void BapSerialCLI::cmdStopsList_() {
  String msg = "stops (gateway): ";
  for (uint8_t i = 0; i < _cfg->num_stops; i++) {
    if (i) msg += ", ";
    msg += _cfg->stops[i];
  }
  if (_cfg->num_stops == 0) msg += "(none)";
  reply_(msg);
}

void BapSerialCLI::cmdStop_(const char* arg) {
  int code = atoi(arg);
  if (code <= 0 || code > 65535) { reply_("invalid stop code"); return; }
  _cfg->client_stop = (uint16_t)code;
  String msg = "client stop: " + String(code);
  reply_(msg);
}

void BapSerialCLI::cmdPoll_(const char* arg) {
  int sec = atoi(arg);
  if (sec < 30) { reply_("min 30s recommended (511.org rate limit)"); return; }
  _cfg->poll_interval_sec = (uint32_t)sec;
  String msg = "poll interval: " + String(sec) + "s";
  reply_(msg);
}

void BapSerialCLI::cmdSave_() {
  if (_cfg->save()) reply_("saved");
  else reply_("save failed");
}

void BapSerialCLI::cmdShow_() {
  Serial.println();
  Serial.print(_cfg->maskedShow());
}

void BapSerialCLI::reply_(const char* msg) {
  Serial.print("  -> ");
  Serial.println(msg);
}

void BapSerialCLI::reply_(const String& msg) {
  reply_(msg.c_str());
}

char* BapSerialCLI::nextToken_(char** saveptr) {
  // Unused; kept for header compat. dispatch_ uses strtok_r directly.
  (void)saveptr;
  return nullptr;
}
