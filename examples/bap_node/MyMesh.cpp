#include "MyMesh.h"
#include "ArrivalDisplay.h"
#include <SHA256.h>

// SHA256("BAP_PUBLIC_CHANNEL_V1") — generated once, hardcoded.
// To regenerate: any sha256 tool with the literal ASCII input.
// This is intentionally published; authenticity comes from Ed25519 sig inside.
static const uint8_t BAP_PUBLIC_SECRET[PUB_KEY_SIZE] = {
  // Placeholders filled in below by SHA256 at init time
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
  0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

// We compute the actual secret at runtime (first call) so we don't have to
// hardcode the SHA256 output and trust that it matches our source string.
static uint8_t bap_secret_cache[PUB_KEY_SIZE];
static bool bap_secret_computed = false;

static void computeBapSecret() {
  if (bap_secret_computed) return;
  SHA256 sha;
  sha.reset();
  const char* input = "BAP_PUBLIC_CHANNEL_V1";
  sha.update((const uint8_t*)input, strlen(input));
  sha.finalize(bap_secret_cache, PUB_KEY_SIZE);
  bap_secret_computed = true;
}

const mesh::GroupChannel BAP_PUBLIC_CHANNEL = {
  .hash = { 0x42 },
  .secret = { /* populated lazily via computeBapSecret() */ }
};

// We can't constinit the secret because SHA256 is runtime. Provide a getter.
static void fillBapChannel(mesh::GroupChannel& out) {
  computeBapSecret();
  out.hash[0] = 0x42;
  memcpy(out.secret, bap_secret_cache, PUB_KEY_SIZE);
}

bool MyMesh::sendBapBroadcast(const uint8_t* data_with_sig, size_t total_len) {
  mesh::GroupChannel ch;
  fillBapChannel(ch);
  mesh::Packet* pkt = createGroupDatagram(PAYLOAD_TYPE_GRP_DATA, ch,
                                          data_with_sig, total_len);
  if (!pkt) return false;
  sendFlood(pkt);
  return true;
}

int MyMesh::searchChannelsByHash(const uint8_t* hash,
                                 mesh::GroupChannel channels[],
                                 int max_matches) {
  if (max_matches < 1) return 0;
  if (hash[0] == 0x42) {
    fillBapChannel(channels[0]);
    return 1;
  }
  // Delegate to base for any other group channels (we don't use any)
  return mesh::Mesh::searchChannelsByHash(hash, channels, max_matches);
}

void MyMesh::onGroupDataRecv(mesh::Packet* packet, uint8_t type,
                             const mesh::GroupChannel& channel,
                             uint8_t* data, size_t len) {
  if (type != PAYLOAD_TYPE_GRP_DATA) return;
  if (len < BAP_HEADER_LEN + BAP_SIG_LEN) return;
  if (data[0] != BAP_TYPE_BYTE) return;

  size_t data_len = len - BAP_SIG_LEN;

  // Verify Ed25519 signature
  if (!_cfg->has_pubkey) return;
  if (!BapPacket::verify(data, data_len, _cfg->pubkey)) return;

  // Decode
  uint16_t stop_code;
  uint8_t n_visits;
  ArrivalVisit visits[BAP_MAX_VISITS];
  const uint8_t* sig_ptr;
  if (!BapPacket::decode(data, len, stop_code, n_visits, visits, sig_ptr)) return;

  // Only display if this packet is for our configured client stop
  if (stop_code != _cfg->client_stop) return;

  if (_display) {
    _display->updateArrivals(stop_code, visits, n_visits);
  }
}
