# BAP-Node Firmware — File-by-File Implementation Plan

Single-binary firmware for the Heltec Vision Master E290 (ESP32-S3, SX1262 LoRa,
296×128 **black-and-white** e-ink). Displays SF Muni bus arrival times.

Two runtime roles selected via serial CLI and stored in SPIFFS JSON config:

- **gateway** — WiFi → bap-http → SIRI parse → Ed25519 sign → LoRa flood + local display
- **client** — LoRa flood → verify signature → local display (no WiFi)

Architecture source: `.hermes/plan.md`

## Resolved Decisions

| # | Item | Value |
|---|---|---|
| 1 | Variant dir | `variants/heltec_e290_bap/` extending `Heltec_E290_base` |
| 2 | Config | `/bap.cfg` JSON in SPIFFS via ArduinoJson v7. Sensitive fields write-only |
| 3 | SIRI source | bap-http returns JSON passthrough of 511.org SIRI |
| 4 | Mesh dispatch | **Option C**: `PAYLOAD_TYPE_GRP_DATA` (0x06) over public group channel |
| 5 | Channel | Hash `0x42`, secret = SHA256("BAP_PUBLIC_CHANNEL_V1") |
| 6 | Client stop | Single `uint16` per client |
| 7 | Signing | Ed25519 via `ed_25519.h` (sign) / `<Ed25519.h>` (verify), matching meshcore |
| 8 | Radio | 910.525 MHz / SF7 / BW62.5 / CR5 / power 22 (USA preset) |
| 9 | Display | BW only (`DARK`/`LIGHT`) |
| 10 | Stop header | `Stop 17056` (numeric; no friendly names) |
| 11 | Poll interval | 60s default; `bap> poll <sec>` configurable; min 30s |
| 12 | Client power | RX always on; no deep sleep in MVP |
| 13 | OTA / battery | Deferred / skipped |
| 14 | Priv key storage | `/bap.key` binary (64-byte meshcore format) |
| 15 | Error UI | Top-of-screen status text |
| 16 | NTP | `configTime(0, 0, "pool.ntp.org")` on gateway |
| 17 | Tests | Native unit tests for `BapPacket` (deferred — see Future Work) |

## Why Option C (Group Datagram) instead of RAW_CUSTOM

`Mesh::onRecvPacket` for `PAYLOAD_TYPE_RAW_CUSTOM` (Mesh.cpp:280) only fires for
DIRECT-route packets — flood-route packets are silently dropped. This blocks
multi-hop relay without an upstream patch.

Group datagrams (`PAYLOAD_TYPE_GRP_DATA`, Mesh.cpp:215-240) flood natively
via `routeRecvPacket()` — full multi-hop support with path accumulation, hop
limits, airtime budgets.

**Trade-off**: meshcore expects encrypted payloads for group datagrams.
Since we publish the channel secret, the "encryption" is theater — but it costs
only microseconds of AES per packet. **Authenticity comes from Ed25519 signing
inside the encrypted payload**, which is real.

## File Layout

```
variants/heltec_e290_bap/
  platformio.ini        - extends Heltec_E290_base
  target.h              - board/radio/display externs (copy of heltec_e290)
  target.cpp            - singleton instances

examples/bap_node/
  main.cpp              - entry point, role branching
  MyMesh.h/.cpp         - mesh::Mesh subclass; group datagram dispatch
  BapConfig.h/.cpp      - /bap.cfg + /bap.key persistence
  BapPacket.h/.cpp      - wire format + Ed25519 sign/verify
  SiriParser.h/.cpp     - defensive SIRI JSON parsing
  GatewayTask.h/.cpp    - WiFi + HTTP polling + broadcast
  ArrivalDisplay.h/.cpp - BW e-ink rendering
  BapSerialCLI.h/.cpp   - bap> command shell
```

## File-by-File Responsibilities

### `variants/heltec_e290_bap/platformio.ini`

Single `[env:bap_node]` target extending `Heltec_E290_base`. Overrides LoRa
radio params for USA preset (910.525 MHz / SF7 / BW62.5 / CR5 / power 22).
Adds `ArduinoJson @ ^7.0.0` to lib_deps along with existing CRC32 and OTA libs.

### `variants/heltec_e290_bap/target.{h,cpp}`

Verbatim copy of `variants/heltec_e290/target.{h,cpp}`. Declares board, radio,
RTC, sensors, display singletons. Avoids upstream conflicts (Decision #1).

### `examples/bap_node/main.cpp`

Entry point following `simple_repeater/main.cpp` pattern:
1. Bring up serial, board, display, radio, RNG, SPIFFS
2. Load or generate mesh identity (IdentityStore → radio_new_identity)
3. Load `/bap.cfg` (optional on first boot)
4. Construct `MyMesh`, wire `ArrivalDisplay`
5. Role branch:
   - Gateway: instantiate `GatewayTask` → `begin()` (WiFi + NTP)
   - Client: set status "Listening..." or "Not configured"
6. `loop()`: CLI → mesh → display → gateway (if applicable) → RTC tick

### `examples/bap_node/BapConfig.{h,cpp}`

Stores all device configuration. Two files in SPIFFS:
- `/bap.cfg` — JSON, versioned, masked on `show`
- `/bap.key` — 64-byte raw Ed25519 private key (meshcore format)

**Schema (v1):**
```json
{
  "version": 1,
  "role": "gateway",
  "wifi_ssid": "...",
  "wifi_pass": "...",
  "endpoint": "http://192.168.1.217:5000",
  "secret": "...",
  "stops": [17056, 13330],
  "client_stop": 17056,
  "poll_interval_sec": 60,
  "pubkey_hex": "a3f7...e2c1"
}
```

Key API:
- `bool load()` / `bool save()`
- `static bool loadPrivKey(...)` / `static bool savePrivKey(...)`
- `String maskedShow()` — replaces `wifi_pass`/`secret` with `***`
- Hex helpers for pubkey serialization

### `examples/bap_node/BapPacket.{h,cpp}`

Wire format encoder/decoder + Ed25519 sign/verify. Stateless.

**Wire format (inside group datagram encrypted payload):**
```
[0]    TYPE = 0xB0 (magic byte, identifies our packets)
[1]    Version = 0x01
[2-3]  Stop code (uint16 big-endian, e.g. 17056)
[4]    Visit count N (≤3)
[5..]  N × { LineRef\0, DestDisplay\0, ETA_byte }
       ETA: 255 = DUE/unknown, else minutes (0..254)
[+64]  Ed25519 signature over [0..data_len-1]
```

Budget: 120 bytes data + 64 sig = 184 (fits MAX_PACKET_PAYLOAD).

Key API:
- `encode(buf, stop_code, visits, n)` → data_len (no sig)
- `sign(buf, data_len, privkey[64])` → appends sig, returns total len
- `verify(buf, data_len, pubkey[32])` → Ed25519::verify
- `decode(buf, total_len, ...)` → parses header + visits + sig ptr

Sign uses `ed25519_sign()` (needs derived pubkey — see Identity.cpp:135-137).
Verify uses `Ed25519::verify()` (only needs pubkey).

### `examples/bap_node/MyMesh.{h,cpp}`

Subclass of `mesh::Mesh`. Two responsibilities:

1. **searchChannelsByHash(hash, out, max)** — when meshcore receives a group
   datagram, it asks this hook "do you have a channel matching hash byte X?".
   For hash `0x42`, fill in `BAP_PUBLIC_CHANNEL` (hash + secret) and return 1.
   meshcore will then decrypt and call our `onGroupDataRecv`.

2. **onGroupDataRecv(packet, type, channel, data, len)** — receives decrypted
   BAP payload. Verifies Ed25519 sig with stored pubkey, decodes visits,
   updates display if packet is for our configured client stop.

3. **sendBapBroadcast(data_with_sig, total_len)** — wraps in group datagram
   via `createGroupDatagram(PAYLOAD_TYPE_GRP_DATA, BAP_PUBLIC_CHANNEL, ...)`
   then `sendFlood()`.

Secret generation: SHA256("BAP_PUBLIC_CHANNEL_V1") computed lazily at first
use. Identical on all BAP firmware builds.

### `examples/bap_node/SiriParser.{h,cpp}`

Defensive JSON parser for bap-http SIRI passthrough. Handles both bare
(`"LineRef": "5"`) and wrapped (`"LineRef": {"value": "5"}`) forms since
bap-http doesn't normalize.

**Field paths (per bap-http README):**
- `MonitoringRef` (top-level per `MonitoredStopVisit`) — stop ID
- `MonitoredVehicleJourney.LineRef` — route number
- `MonitoredVehicleJourney.DestinationName` — destination
- `MonitoredVehicleJourney.MonitoredCall.ExpectedArrivalTime` — ETA
  - Fallback chain: ExpectedDepartureTime → AimedArrivalTime → AimedDepartureTime

Time format: ISO-8601 UTC ("2026-07-20T00:06:43Z"). Parsed via sscanf +
mktime (gateway runs NTP/UTC).

Output: top-3 visits sorted by ETA ascending, ETA capped at 254 min (255 = DUE).

### `examples/bap_node/GatewayTask.{h,cpp}`

Manages gateway lifecycle:
1. `begin()` — load `/bap.key`, start WiFi STA + auto-reconnect, NTP sync
2. `loop()` — non-blocking; periodic HTTP poll per `poll_interval_sec`
3. Per stop: GET `<endpoint>/stopmonitoring?stopcode=<code>` with
   `Authorization: Bearer <secret>`, parse with SiriParser, broadcast
   signed packet via mesh, update local display (for stops[0])

HTTP 503 (`stale_data` from bap-http) → set status "HTTP 503", don't broadcast.
Missing privkey/pubkey/stops → status text, no polling.

### `examples/bap_node/ArrivalDisplay.{h,cpp}`

Wraps `E290Display` (the `DisplayDriver` instance from target.h). BW only.

**Layout (296×128):**
```
Stop 17056                          ← header (16px)
  5  to Transit Center       3m     ← visit row (37px × 3)
 45  to Lyon + Greenwich    12m
  N  to Caltrain            DUE
```

- `setStatus("WiFi DOWN" | "HTTP 503" | "BAD SIG" | ...)` — top-of-screen
- `updateArrivals(stop_code, visits, n)` — replaces data, marks dirty
- `loop()` — redraws on dirty (e-ink is slow; coalesces rapid updates)

Status text always renders even if no arrivals yet.

### `examples/bap_node/BapSerialCLI.{h,cpp}`

`bap> ` shell over USB CDC. Tokenizer handles `"double-quoted"` args (for
WiFi passwords with spaces). Commands:

- `role gateway|client`
- `wifi SSID "pass"`
- `endpoint "http://host:port"`
- `secret "token"`
- `keygen` (gateway only — generates Ed25519 keypair, saves /bap.key)
- `pubkey <64-hex>` (client — imports gateway's public key)
- `stops add <code>` / `stops del <code>` / `stops list` (gateway)
- `stop <code>` (client — single)
- `poll <sec>` (default 60, min 30 enforced)
- `save` / `show` / `help`

Keygen uses `_mesh->getRNG()` (meshcore's radio-noise-seeded RNG) via
`mesh::LocalIdentity(rng)`.

## Dependency Graph

```
platformio.ini ──► Heltec_E290_base ──► heltec-eink-modules + RadioLib + Crypto
                                              │
target.{h,cpp} ──► (board/radio/display singletons)
                                              │
                                              ▼
main.cpp ──► BapConfig ──► SPIFFS + ArduinoJson
    │           │
    │           ├──► /bap.key (binary)
    │           │
    ├──► MyMesh ──► BapPacket ──► Ed25519 (sign via ed_25519.h, verify via Ed25519.h)
    │      │
    │      ├──► BAP_PUBLIC_CHANNEL (SHA256 of constant string)
    │      │
    │      └──► ArrivalDisplay (via setDisplay)
    │              │
    │              └──► E290Display (BW DisplayDriver)
    │
    ├──► GatewayTask (gateway only) ──► SiriParser ──► HTTPClient + WiFi
    │      │
    │      └──► MyMesh.sendBapBroadcast → createGroupDatagram → sendFlood
    │
    └──► BapSerialCLI ──► BapConfig + MyMesh.getRNG
```

## Implementation Phases

### Phase 1 — Scaffolding ✅
- `variants/heltec_e290_bap/` with platformio.ini, target.h/.cpp
- `examples/bap_node/main.cpp` skeleton
- Compiles, boots, serial echo works

### Phase 2 — Config + CLI ✅
- `BapConfig.{h,cpp}` — JSON load/save + masked show
- `BapSerialCLI.{h,cpp}` — all bap> commands
- `bap> save/show/role/wifi/...` round-trips through SPIFFS

### Phase 3 — Gateway: WiFi + HTTP + SIRI ✅
- `GatewayTask.{h,cpp}` — WiFi, NTP, HTTP poll loop
- `SiriParser.{h,cpp}` — defensive parse (bare or wrapped fields)
- 503 stale handling, status UI updates

### Phase 4 — Mesh Protocol ✅
- `BapPacket.{h,cpp}` — wire format + Ed25519
- `MyMesh.{h,cpp}` — group datagram dispatch + sendBapBroadcast
- Multi-hop relay works natively via meshcore's flood routing

### Phase 5 — Display ✅
- `ArrivalDisplay.{h,cpp}` — header, 3 visit rows, status text
- Dirty-flag redraw (coalesces rapid updates)

### Phase 6 — Polish (deferred)
- OTA updates (lib_deps already wired)
- Battery monitoring (HeltecE290Board::getBattMilliVolts is wired)
- Deep sleep for client (board.sleep pattern from simple_repeater)
- Native unit tests for BapPacket encode/decode/verify
- Real bap-http integration testing (need live endpoint + sample responses)

## Patterns Followed

| Concern | Reference |
|---|---|
| Variant structure | `variants/heltec_e290/platformio.ini` `[env:Heltec_E290_room_server]` |
| Minimal `main.cpp` | `examples/simple_repeater/main.cpp` |
| Mesh subclass | `examples/simple_room_server/MyMesh.h` (simplified) |
| SPIFFS + identity init | `simple_room_server/main.cpp:40-64` |
| Ed25519 sign (64-byte prv) | `src/Identity.cpp:135-137` (`ed25519_sign` from `<ed_25519.h>`) |
| Ed25519 verify (32-byte pub) | `src/Identity.cpp:22` (`Ed25519::verify` from `<Ed25519.h>`) |
| WiFi connect + auto-reconnect | `examples/companion_radio/main.cpp:177-216` |
| Group datagram dispatch | `src/Mesh.cpp:215-240` (PAYLOAD_TYPE_GRP_DATA case) |
| Group channel interface | `src/Mesh.h:7-11` (GroupChannel struct) |
| E290 display API | `src/helpers/ui/E290Display.h` |
| Board API (battery, etc.) | `variants/heltec_e290/HeltecE290Board.h` |
| StaticPoolPacketManager size | `examples/simple_repeater/MyMesh.cpp:850` (32) |

## bap-http Integration Notes

- **Endpoint**: `GET <base>/stopmonitoring?stopcode=<code>`
- **Auth**: `Authorization: Bearer <secret>` header
- **Response**: SIRI passthrough JSON — visits are unchanged from 511.org
- **Stale signal**: HTTP 503 `{error: "stale_data", ...}` when cache > 120s
- **Rate limit**: 60 req/hour shared; we poll once per `poll_interval_sec` per stop
- **Time format**: ISO-8601 UTC with `Z` suffix
- **BOM**: bap-http strips UTF-8 BOM from upstream 511.org responses
- **Layout**: accepts both `{"Siri":{"ServiceDelivery":...}}` and flat `{"ServiceDelivery":...}`

## Open Follow-ups

1. **Live integration test** with real bap-http endpoint (need network + hardware)
2. **Field-path verification** — SiriParser is defensive but the exact wrapping
   (`{"value":"5"}` vs `"5"`) of 511.org's response needs verification against
   live data; defensive parsing handles both
3. **Native unit tests** — `BapPacket` encode/decode/verify round-trip would
   catch binary format bugs early. Requires adding Ed25519 + ed25519 source
   to `[env:native]` build_src_filter in root `platformio.ini`
4. **Multi-stop gateway display** — currently only `stops[0]` shown on gateway
   display; could rotate
