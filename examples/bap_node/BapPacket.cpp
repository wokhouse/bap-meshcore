#include "BapPacket.h"
#include <string.h>
#include <Ed25519.h>
// ed_25519.h provides ed25519_sign/ed25519_derive_pub with the 64-byte private
// key format meshcore uses (see src/Identity.cpp:135-137).
#define ED25519_NO_SEED 1
#include <ed_25519.h>

size_t BapPacket::encode(uint8_t* buf, uint16_t stop_code,
                         const ArrivalVisit* visits, uint8_t n_visits) {
  if (n_visits > BAP_MAX_VISITS) n_visits = BAP_MAX_VISITS;

  size_t i = 0;
  buf[i++] = BAP_TYPE_BYTE;
  buf[i++] = BAP_VERSION;
  buf[i++] = (stop_code >> 8) & 0xFF;     // big-endian
  buf[i++] = stop_code & 0xFF;
  buf[i++] = n_visits;

  for (uint8_t v = 0; v < n_visits; v++) {
    // LineRef (null-terminated)
    size_t lr_len = strnlen(visits[v].line_ref, BAP_LINE_REF_MAX - 1);
    if (i + lr_len + 1 > BAP_DATA_BUDGET) return 0;
    memcpy(&buf[i], visits[v].line_ref, lr_len);
    i += lr_len;
    buf[i++] = '\0';

    // DestDisplay (null-terminated, truncated if needed)
    size_t dest_len = strnlen(visits[v].dest, BAP_DEST_MAX - 1);
    // Truncate dest if it would overflow the data budget (leave room for \0 + eta + sig)
    size_t remaining = BAP_DATA_BUDGET - i - 1;  // -1 for null
    if (dest_len > remaining) dest_len = remaining;
    memcpy(&buf[i], visits[v].dest, dest_len);
    i += dest_len;
    buf[i++] = '\0';

    // ETA byte
    if (i + 1 > BAP_DATA_BUDGET) return 0;
    buf[i++] = visits[v].eta_min;
  }

  return i;  // data_len (no signature)
}

size_t BapPacket::sign(uint8_t* buf, size_t data_len, const uint8_t privkey[64]) {
  if (data_len == 0) return 0;
  // ed25519_sign requires the public key alongside the private key.
  // Derive it from the private key (32-byte output).
  uint8_t pubkey[32];
  ed25519_derive_pub(pubkey, privkey);
  ed25519_sign(&buf[data_len], buf, data_len, pubkey, privkey);
  return data_len + BAP_SIG_LEN;
}

bool BapPacket::verify(const uint8_t* buf, size_t data_len, const uint8_t pubkey[32]) {
  if (data_len < BAP_HEADER_LEN) return false;
  if (buf[0] != BAP_TYPE_BYTE) return false;
  if (buf[1] != BAP_VERSION) return false;
  return Ed25519::verify(&buf[data_len], pubkey, buf, data_len);
}

bool BapPacket::decode(const uint8_t* buf, size_t total_len,
                       uint16_t& stop_code, uint8_t& n_visits,
                       ArrivalVisit* visits, const uint8_t*& sig_ptr) {
  if (total_len < BAP_HEADER_LEN + BAP_SIG_LEN) return false;
  if (buf[0] != BAP_TYPE_BYTE) return false;
  if (buf[1] != BAP_VERSION) return false;

  stop_code = ((uint16_t)buf[2] << 8) | buf[3];
  n_visits = buf[4];
  if (n_visits > BAP_MAX_VISITS) return false;

  size_t i = BAP_HEADER_LEN;
  size_t data_end = total_len - BAP_SIG_LEN;

  for (uint8_t v = 0; v < n_visits; v++) {
    if (i >= data_end) return false;

    // LineRef
    size_t lr_len = 0;
    while (i + lr_len < data_end && buf[i + lr_len] != '\0') lr_len++;
    if (i + lr_len >= data_end) return false;  // no null terminator
    if (lr_len >= BAP_LINE_REF_MAX) lr_len = BAP_LINE_REF_MAX - 1;
    memcpy(visits[v].line_ref, &buf[i], lr_len);
    visits[v].line_ref[lr_len] = '\0';
    i += lr_len + 1;

    // DestDisplay
    if (i >= data_end) return false;
    size_t dest_len = 0;
    while (i + dest_len < data_end && buf[i + dest_len] != '\0') dest_len++;
    if (i + dest_len >= data_end) return false;
    if (dest_len >= BAP_DEST_MAX) dest_len = BAP_DEST_MAX - 1;
    memcpy(visits[v].dest, &buf[i], dest_len);
    visits[v].dest[dest_len] = '\0';
    i += dest_len + 1;

    // ETA
    if (i >= data_end) return false;
    visits[v].eta_min = buf[i++];
  }

  sig_ptr = &buf[data_end];
  return true;
}
