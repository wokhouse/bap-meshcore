#include "ArrivalDisplay.h"
#include <stdio.h>

// Layout for 296x128 E290:
//   y=0..15:    stop header ("Stop 17056")
//   y=16..127:  3 visit rows (~37px each, big font)
static const int HEADER_H = 16;
static const int ROW_H = 37;

void ArrivalDisplay::begin() {
  showSplash("BAP Node\nStarting...");
}

void ArrivalDisplay::beginGateway() {
  showSplash("BAP Gateway\nInitializing...");
}

void ArrivalDisplay::showSplash(const char* msg) {
  _disp->startFrame();
  _disp->setCursor(0, 0);
  _disp->setTextSize(1);
  _disp->print(msg);
  _disp->endFrame();
}

void ArrivalDisplay::updateArrivals(uint16_t stop_code, const ArrivalVisit* visits, uint8_t n) {
  if (n > BAP_MAX_VISITS) n = BAP_MAX_VISITS;
  _stop_code = stop_code;
  _n_visits = n;
  for (uint8_t i = 0; i < n; i++) _visits[i] = visits[i];
  _has_data = true;
  _last_update_millis = millis();
  _status[0] = '\0';
  _dirty = true;
}

void ArrivalDisplay::setStatus(const char* status) {
  if (status) {
    strncpy(_status, status, BAPDisp_STATUS_MAX - 1);
    _status[BAPDisp_STATUS_MAX - 1] = '\0';
  } else {
    _status[0] = '\0';
  }
  _dirty = true;
}

void ArrivalDisplay::loop() {
  if (!_dirty) return;
  _dirty = false;
  render_();
}

void ArrivalDisplay::render_() {
  _disp->startFrame();
  _disp->clear();

  // Only draw the stop header once we actually have a stop to show — otherwise
  // boot shows a meaningless "Stop 0" before the first poll.
  if (_has_data) {
    renderHeader_(0);
  }

  int y = HEADER_H;
  if (_has_data && _n_visits > 0) {
    for (uint8_t i = 0; i < _n_visits; i++) {
      renderVisit_(y, i);
      y += ROW_H;
    }
  } else {
    // No data yet: show status (e.g. "NTP SYNC...", "Listening...") or a
    // neutral placeholder. Centered-ish in the visit area.
    _disp->setCursor(0, y + 8);
    _disp->setTextSize(1);
    _disp->print(_status[0] ? _status : "No arrivals");
  }

  renderStatus_(128 - 12);

  _disp->endFrame();
}

void ArrivalDisplay::renderHeader_(int y) {
  _disp->setCursor(0, y);
  _disp->setTextSize(1);
  char buf[24];
  snprintf(buf, sizeof(buf), "Stop %u", _stop_code);
  _disp->print(buf);
}

void ArrivalDisplay::renderVisit_(int y, uint8_t idx) {
  const ArrivalVisit& v = _visits[idx];
  char buf[64];

  _disp->setCursor(0, y + 8);
  _disp->setTextSize(2);
  // "  5  " padded left so column lines up for 1- and 2-char route numbers
  snprintf(buf, sizeof(buf), "%2s", v.line_ref);
  _disp->print(buf);

  _disp->setCursor(48, y + 8);
  _disp->setTextSize(1);
  _disp->print("to");

  _disp->setCursor(64, y + 8);
  // Truncate destination to fit 296-64-60 px (rough estimate)
  snprintf(buf, sizeof(buf), "%.*s", 20, v.dest);
  _disp->print(buf);

  // ETA right-aligned at x ~240
  if (v.eta_min == 255) {
    snprintf(buf, sizeof(buf), "DUE");
  } else {
    snprintf(buf, sizeof(buf), "%dm", v.eta_min);
  }
  _disp->setCursor(240, y + 8);
  _disp->setTextSize(2);
  _disp->print(buf);
}

void ArrivalDisplay::renderStatus_(int y) {
  if (!_status[0]) return;
  _disp->setCursor(0, y);
  _disp->setTextSize(1);
  _disp->print(_status);
}
