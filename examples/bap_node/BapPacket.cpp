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

  // BAP_DATA_BUDGET is the space for header+visits BEFORE the signature; the
  // signature is already subtracted out in the budget derivation (see
  // BapPacket.h). So everything below must fit header+visits into exactly
  // BAP_DATA_BUDGET.
  //
  // Two-pass design. The old single-pass loop truncated each destination
  // against only the bytes remaining at that moment, ignoring the current
  // visit's ETA byte and all later visits. With 3 full-length destinations
  // (5 + 3*37 = 116 > 104) it wrote visits 1-2 up to the budget edge, then
  // bailed on the final ETA — dropping the ENTIRE packet instead of trimming
  // destinations. Result: the gateway broadcast nothing for a perfectly normal
  // 3-arrival response.

  // --- Pass 1: measure fixed overhead and each destination's natural length.
  size_t lineref_len[BAP_MAX_VISITS];
  size_t dest_natural[BAP_MAX_VISITS];
  size_t dest_len[BAP_MAX_VISITS];

  size_t overhead = BAP_HEADER_LEN;   // TYPE+VER+stopcode(2)+visit_count
  for (uint8_t v = 0; v < n_visits; v++) {
    lineref_len[v]  = strnlen(visits[v].line_ref, BAP_LINE_REF_MAX - 1);
    dest_natural[v] = strnlen(visits[v].dest, BAP_DEST_MAX - 1);
    // Fixed per-visit cost: lineref bytes + null + dest bytes + null + eta.
    overhead += lineref_len[v] + 3;
  }
  // Defensive: if even the empty-dest structure can't fit, give up. With
  // BAP_MAX_VISITS=3 this never trips (~23 bytes), but guards a future bump.
  if (overhead > BAP_DATA_BUDGET) return 0;
  size_t dest_budget = BAP_DATA_BUDGET - overhead;

  // --- Pass 2: distribute dest_budget across visits with max-min fairness.
  // Short destinations keep their full text; only destinations that are
  // collectively too long get trimmed. This guarantees 3 visits always encode
  // (e.g. "Ferry Building" + "Embarcadero Station" + "SF State University")
  // rather than failing the whole broadcast. O(budget*n) with budget<=~90 and
  // n<=3 — trivial on an ESP32-S3, and obviously correct by construction.
  for (uint8_t v = 0; v < n_visits; v++) dest_len[v] = 0;
  while (dest_budget > 0) {
    uint8_t best = 0xFF;
    size_t best_alloc = SIZE_MAX;
    for (uint8_t v = 0; v < n_visits; v++) {
      if (dest_len[v] < dest_natural[v] && dest_len[v] < best_alloc) {
        best_alloc = dest_len[v];
        best = v;
      }
    }
    if (best == 0xFF) break;   // every destination is already at full length
    dest_len[best]++;
    dest_budget--;
  }

  // --- Emit.
  size_t i = 0;
  buf[i++] = BAP_TYPE_BYTE;
  buf[i++] = BAP_VERSION;
  buf[i++] = (stop_code >> 8) & 0xFF;     // big-endian
  buf[i++] = stop_code & 0xFF;
  buf[i++] = n_visits;

  for (uint8_t v = 0; v < n_visits; v++) {
    memcpy(&buf[i], visits[v].line_ref, lineref_len[v]);
    i += lineref_len[v];
    buf[i++] = '\0';

    memcpy(&buf[i], visits[v].dest, dest_len[v]);
    i += dest_len[v];
    buf[i++] = '\0';

    buf[i++] = visits[v].eta_min;
  }

  return i;  // data_len (no signature); guaranteed <= BAP_DATA_BUDGET
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

// Walk the header+visits structure to compute the exact number of signed bytes.
// This is needed because the decrypted buffer is padded up to a 16-byte AES
// block boundary by meshcore's cipher (Utils::encrypt rounds up, Utils::decrypt
// returns the full block-rounded length). The signature was made over the
// *unpadded* plaintext, so we must reconstruct that length from the packet
// structure rather than trusting the buffer length. Returns 0 on malformed input.
static size_t signedLen_(const uint8_t* buf, size_t buf_len) {
  if (buf_len < BAP_HEADER_LEN + BAP_SIG_LEN) return 0;
  if (buf[0] != BAP_TYPE_BYTE) return 0;
  if (buf[1] != BAP_VERSION) return 0;
  uint8_t n_visits = buf[4];
  if (n_visits > BAP_MAX_VISITS) return 0;

  size_t i = BAP_HEADER_LEN;
  size_t sig_start = buf_len - BAP_SIG_LEN;   // earliest the sig can begin

  for (uint8_t v = 0; v < n_visits; v++) {
    // LineRef: scan to null terminator
    size_t lr_len = 0;
    while (i + lr_len < sig_start && buf[i + lr_len] != '\0') lr_len++;
    if (i + lr_len >= sig_start) return 0;     // ran into signature without null
    i += lr_len + 1;

    // DestDisplay: scan to null terminator
    size_t dest_len = 0;
    while (i + dest_len < sig_start && buf[i + dest_len] != '\0') dest_len++;
    if (i + dest_len >= sig_start) return 0;
    i += dest_len + 1;

    // ETA
    if (i >= sig_start) return 0;
    i++;
  }
  return i;   // exact number of bytes that were signed
}

bool BapPacket::verifySigned(const uint8_t* buf, size_t buf_len, const uint8_t pubkey[32]) {
  size_t data_len = signedLen_(buf, buf_len);
  if (data_len == 0) return false;
  return Ed25519::verify(&buf[data_len], pubkey, buf, data_len);
}

bool BapPacket::decode(const uint8_t* buf, size_t total_len,
                       uint16_t& stop_code, uint8_t& n_visits,
                       ArrivalVisit* visits, const uint8_t*& sig_ptr) {
  // Recover the exact signed length (total_len may include AES block padding).
  size_t data_len = signedLen_(buf, total_len);
  if (data_len == 0) return false;

  stop_code = ((uint16_t)buf[2] << 8) | buf[3];
  n_visits = buf[4];

  size_t i = BAP_HEADER_LEN;
  for (uint8_t v = 0; v < n_visits; v++) {
    // LineRef
    size_t lr_len = 0;
    while (i + lr_len < data_len && buf[i + lr_len] != '\0') lr_len++;
    if (lr_len >= BAP_LINE_REF_MAX) lr_len = BAP_LINE_REF_MAX - 1;
    memcpy(visits[v].line_ref, &buf[i], lr_len);
    visits[v].line_ref[lr_len] = '\0';
    i += lr_len + 1;

    // DestDisplay
    size_t dest_len = 0;
    while (i + dest_len < data_len && buf[i + dest_len] != '\0') dest_len++;
    if (dest_len >= BAP_DEST_MAX) dest_len = BAP_DEST_MAX - 1;
    memcpy(visits[v].dest, &buf[i], dest_len);
    visits[v].dest[dest_len] = '\0';
    i += dest_len + 1;

    // ETA
    visits[v].eta_min = buf[i++];
  }

  sig_ptr = &buf[data_len];
  return true;
}
