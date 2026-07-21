#pragma once

#include <Arduino.h>
#include "BapPacket.h"

// SIRI field-path defensive parser.
//
// bap-http passes 511.org SIRI JSON through unchanged. SIRI profiling
// implementations may emit e.g. LineRef as either:
//   "LineRef": "5"            (bare string)
//   "LineRef": {"value": "5"} (wrapped object)
// SiriParser handles both.
//
// Field paths (per bap-http README):
//   LineRef:           MonitoredVehicleJourney.LineRef
//   Destination:       MonitoredVehicleJourney.DestinationName
//   ETA:               MonitoredVehicleJourney.MonitoredCall.ExpectedArrivalTime
//                      (fallback: ExpectedDepartureTime -> AimedArrivalTime -> AimedDepartureTime)
//   Stop ID:           MonitoringRef  (top-level on each MonitoredStopVisit)
class SiriParser {
public:
  // Parse bap-http JSON response, extract top-3 arrivals for want_stop, sorted by ETA ascending.
  // Returns visit count (0..3). Stops not matching want_stop are ignored.
  // Fills out_visits and (if non-null) sets response_ok=false if JSON indicates error.
  static uint8_t parse(const String& json, uint16_t want_stop,
                       ArrivalVisit out_visits[BAP_MAX_VISITS],
                       bool* response_ok = nullptr);

  // Decode an ISO-8601 timestamp ("2026-07-20T00:06:43Z" or with offset) to unix seconds.
  // Returns 0 on failure.
  static uint32_t parseIsoTime(const char* iso);
};
