#pragma once

#include <Arduino.h>
#include <helpers/ui/DisplayDriver.h>
#include "BapPacket.h"

#define BAPDisp_STOP_NAME_MAX   32
#define BAPDisp_STATUS_MAX      24

class ArrivalDisplay {
  DisplayDriver* _disp;
  unsigned long _last_update_millis = 0;
  bool _has_data = false;
  uint16_t _stop_code = 0;
  uint8_t _n_visits = 0;
  ArrivalVisit _visits[BAP_MAX_VISITS];
  char _status[BAPDisp_STATUS_MAX] = {0};
  bool _dirty = true;          // redraw needed

public:
  ArrivalDisplay(DisplayDriver& disp) : _disp(&disp) {}

  void begin();

  // Update arrivals from mesh or HTTP. Triggers a redraw.
  void updateArrivals(uint16_t stop_code, const ArrivalVisit* visits, uint8_t n);

  // Set transient status string ("WiFi DOWN", "HTTP 503", "BAD SIG", "—").
  // Pass nullptr/empty to clear.
  void setStatus(const char* status);

  void showSplash(const char* msg);

  // Call from loop(): redraw if dirty. Throttled by display type (e-ink is slow).
  void loop();

  bool hasData() const { return _has_data; }
  unsigned long lastUpdateMillis() const { return _last_update_millis; }

private:
  void render_();
  void renderHeader_(int y);
  void renderVisit_(int y, uint8_t idx);
  void renderStatus_(int y);
};
