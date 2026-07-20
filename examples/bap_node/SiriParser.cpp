#include "SiriParser.h"
#include <ArduinoJson.h>
#include <time.h>

// Helper: get a string field that may be bare or {"value": "..."}
static const char* getField(const JsonVariant& parent, const char* key) {
  if (!parent.containsKey(key)) return "";
  JsonVariant v = parent[key];
  if (v.is<const char*>()) return v.as<const char*>();
  if (v.is<JsonObject>()) {
    JsonObject obj = v.as<JsonObject>();
    if (obj.containsKey("value")) {
      JsonVariant val = obj["value"];
      if (val.is<const char*>()) return val.as<const char*>();
    }
  }
  return "";
}

// Pick the first available timestamp field from a MonitoredCall (fallback chain per bap-http README)
static const char* getArrivalTime(JsonObject monitored_call) {
  const char* keys[] = {
    "ExpectedArrivalTime",
    "ExpectedDepartureTime",
    "AimedArrivalTime",
    "AimedDepartureTime",
    nullptr
  };
  for (int i = 0; keys[i]; i++) {
    if (monitored_call.containsKey(keys[i])) {
      JsonVariant v = monitored_call[keys[i]];
      if (v.is<const char*>()) {
        const char* s = v.as<const char*>();
        if (s[0]) return s;
      } else if (v.is<JsonObject>()) {
        JsonObject obj = v.as<JsonObject>();
        if (obj.containsKey("value")) {
          JsonVariant val = obj["value"];
          if (val.is<const char*>()) {
            const char* s = val.as<const char*>();
            if (s[0]) return s;
          }
        }
      }
    }
  }
  return "";
}

uint8_t SiriParser::parse(const String& json, uint16_t want_stop,
                          ArrivalVisit out_visits[BAP_MAX_VISITS],
                          bool* response_ok) {
  if (response_ok) *response_ok = false;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, json);
  if (err) return 0;

  // bap-http error envelope
  if (doc.containsKey("error")) return 0;

  if (response_ok) *response_ok = true;

  // Find ServiceDelivery (accept both flat and Siri-wrapped layouts)
  JsonObject service_delivery;
  if (doc.containsKey("ServiceDelivery")) {
    service_delivery = doc["ServiceDelivery"].as<JsonObject>();
  } else if (doc.containsKey("Siri") && doc["Siri"].containsKey("ServiceDelivery")) {
    service_delivery = doc["Siri"]["ServiceDelivery"].as<JsonObject>();
  } else {
    return 0;
  }

  JsonObject smd = service_delivery["StopMonitoringDelivery"].as<JsonObject>();
  if (!smd) return 0;

  JsonArray visits = smd["MonitoredStopVisit"].as<JsonArray>();
  if (!visits) return 0;

  // Collect candidates for want_stop
  ArrivalVisit candidates[BAP_MAX_VISITS];
  uint8_t ncand = 0;
  uint32_t now_sec = time(nullptr);

  for (JsonVariant v : visits) {
    JsonObject visit = v.as<JsonObject>();
    if (!visit) continue;

    const char* mref_str = getField(visit, "MonitoringRef");
    if (!mref_str[0]) continue;

    // Match the requested stop code
    char wantbuf[12];
    snprintf(wantbuf, sizeof(wantbuf), "%u", want_stop);
    if (strcmp(mref_str, wantbuf) != 0) continue;

    JsonObject mvj = visit["MonitoredVehicleJourney"].as<JsonObject>();
    if (!mvj) continue;

    if (ncand >= BAP_MAX_VISITS) break;
    ArrivalVisit& av = candidates[ncand++];

    const char* lineref = getField(mvj, "LineRef");
    strncpy(av.line_ref, lineref, BAP_LINE_REF_MAX - 1);
    av.line_ref[BAP_LINE_REF_MAX - 1] = '\0';

    const char* dest = getField(mvj, "DestinationName");
    strncpy(av.dest, dest, BAP_DEST_MAX - 1);
    av.dest[BAP_DEST_MAX - 1] = '\0';

    JsonObject mc = mvj["MonitoredCall"].as<JsonObject>();
    const char* ts = mc ? getArrivalTime(mc) : "";
    uint32_t arrive_sec = parseIsoTime(ts);
    if (arrive_sec == 0 || now_sec == 0) {
      av.eta_min = 255;   // unknown -> DUE
    } else if (arrive_sec <= now_sec) {
      av.eta_min = 255;   // arrived -> DUE
    } else {
      uint32_t delta = (arrive_sec - now_sec) / 60;
      av.eta_min = (delta > 254) ? 254 : (uint8_t)delta;
    }
  }

  // Sort by eta ascending (simple insertion sort, n<=3)
  for (uint8_t i = 1; i < ncand; i++) {
    ArrivalVisit tmp = candidates[i];
    int8_t j = i - 1;
    while (j >= 0 && candidates[j].eta_min > tmp.eta_min) {
      candidates[j + 1] = candidates[j];
      j--;
    }
    candidates[j + 1] = tmp;
  }

  memcpy(out_visits, candidates, ncand * sizeof(ArrivalVisit));
  return ncand;
}

uint32_t SiriParser::parseIsoTime(const char* iso) {
  if (!iso || !iso[0]) return 0;
  // strptime not always available; do a manual parse for "YYYY-MM-DDTHH:MM:SS" prefix
  struct tm t;
  memset(&t, 0, sizeof(t));
  // YYYY-MM-DDTHH:MM:SS
  if (sscanf(iso, "%d-%d-%dT%d:%d:%d",
             &t.tm_year, &t.tm_mon, &t.tm_mday,
             &t.tm_hour, &t.tm_min, &t.tm_sec) < 6) {
    return 0;
  }
  t.tm_year -= 1900;
  t.tm_mon -= 1;

  // Detect trailing Z (UTC) — assume UTC either way since bap-http returns UTC
  time_t epoch = mktime(&t);
  if (epoch == (time_t)-1) return 0;

  // mktime uses local timezone; for ESP32 with configTime(0,...) this is UTC.
  // If there's an offset like +HH:MM we ignore it (gateway uses NTP/UTC).
  return (uint32_t)epoch;
}
