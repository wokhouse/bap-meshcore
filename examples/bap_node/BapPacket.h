#pragma once

#include <stdint.h>
#include <stddef.h>
#include <Arduino.h>

#define BAP_TYPE_BYTE        0xB0   // magic byte at start of payload
#define BAP_VERSION          0x01
#define BAP_MAX_VISITS       3
#define BAP_SIG_LEN          64     // Ed25519 signature
#define BAP_LINE_REF_MAX     4      // "5", "45", "N" + null
#define BAP_DEST_MAX         32     // destination string + null
#define BAP_HEADER_LEN       5      // TYPE + VER + stop_code(2) + visit_count
#define BAP_DATA_BUDGET      120    // MAX_PACKET_PAYLOAD(184) - SIG(64)

struct ArrivalVisit {
  char line_ref[BAP_LINE_REF_MAX];   // e.g. "5", "45", "N"
  char dest[BAP_DEST_MAX];           // e.g. "Transit Center"
  uint8_t eta_min;                   // minutes (255 = DUE / at stop)
};

class BapPacket {
public:
  // Encode header + visits (NO signature). Returns data_len written to buf, or 0 on overflow.
  // buf must be at least BAP_DATA_BUDGET bytes.
  static size_t encode(uint8_t* buf, uint16_t stop_code,
                       const ArrivalVisit* visits, uint8_t n_visits);

  // Sign data in-place: appends Ed25519 signature at buf[data_len..data_len+64).
  // Returns total length (data_len + 64) or 0 on error.
  static size_t sign(uint8_t* buf, size_t data_len,
                     const uint8_t privkey[64]);

  // Verify signature over buf[0..data_len), signature at buf[data_len..data_len+64).
  static bool verify(const uint8_t* buf, size_t data_len,
                     const uint8_t pubkey[32]);

  // Decode header + visits from a verified buffer.
  // sig_ptr set to start of 64-byte signature (not copied).
  // Returns true on success. Out params: stop_code, n_visits, visits[].
  static bool decode(const uint8_t* buf, size_t total_len,
                     uint16_t& stop_code, uint8_t& n_visits,
                     ArrivalVisit* visits, const uint8_t*& sig_ptr);

  // Convenience: total wire length for n_visits of given dest lengths.
  static size_t wireLen(uint8_t n_visits, ...);
};
