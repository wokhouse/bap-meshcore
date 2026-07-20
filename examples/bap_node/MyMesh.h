#pragma once

#include <Arduino.h>
#include <Mesh.h>
#include <helpers/SimpleMeshTables.h>
#include <helpers/StaticPoolPacketManager.h>
#include <helpers/ArduinoHelpers.h>
#include <helpers/AutoDiscoverRTCClock.h>

#include "BapConfig.h"
#include "BapPacket.h"

// BAP public channel — deterministic, identical on all BAP firmware builds.
// Hash byte 0x42 (avoid 0x00/0xFF reserved for id hashes).
// Secret = SHA256("BAP_PUBLIC_CHANNEL_V1") (32 bytes — full SHA256 output).
//
// The "encryption" provided by meshcore's group channel is theater since the
// secret is published. Authenticity comes from Ed25519 signing inside the
// encrypted payload.
extern const mesh::GroupChannel BAP_PUBLIC_CHANNEL;

class MyMesh : public mesh::Mesh {
  BapConfig* _cfg;
  class ArrivalDisplay* _display;

public:
  MyMesh(mesh::MainBoard& board, mesh::Radio& radio, mesh::MillisecondClock& ms,
         mesh::RNG& rng, mesh::RTCClock& rtc, mesh::MeshTables& tables,
         BapConfig& cfg)
    : mesh::Mesh(radio, ms, rng, rtc, *new StaticPoolPacketManager(32), tables),
      _cfg(&cfg), _display(nullptr) {}

  void setDisplay(ArrivalDisplay* d) { _display = d; }

  // Broadcast a signed BAP packet (header + visits + sig) over the mesh.
  // Returns true on success.
  bool sendBapBroadcast(const uint8_t* data_with_sig, size_t total_len);

  // Called by meshcore when an encrypted group packet arrives (native multi-hop).
  // Decodes our BAP packet, verifies signature, dispatches to display if for us.
  void onGroupDataRecv(mesh::Packet* packet, uint8_t type,
                       const mesh::GroupChannel& channel,
                       uint8_t* data, size_t len) override;

  // Called by meshcore to look up a GroupChannel by hash byte.
  // We return the BAP_PUBLIC_CHANNEL for hash 0x42 so meshcore can decrypt
  // BAP broadcasts (then forwards the decrypted data to onGroupDataRecv).
  int searchChannelsByHash(const uint8_t* hash,
                           mesh::GroupChannel channels[],
                           int max_matches) override;
};
