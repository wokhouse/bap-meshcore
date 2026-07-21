# bap-meshcore

Firmware for displaying SF Muni bus arrival times on a Heltec Vision Master
E290 e-ink display, with one device fetching data over WiFi and broadcasting
it over a LoRa mesh to any number of battery-only client displays in range.

One binary, two runtime roles selected at provisioning time:

- **Gateway** — connects WiFi, polls a [bap-http](../bap-http) instance every
  60s, parses SIRI JSON, signs each packet with Ed25519, floods over LoRa.
- **Client** — listens on LoRa, verifies signatures, renders arrivals.
  No WiFi, no credentials, no upstream dependency.

Multi-hop relay comes free via meshcore's group-datagram flood routing — a
client multiple hops away from the gateway still receives broadcasts as long
as intermediate BAP nodes are powered on.

![Architecture](docs/_assets/bap-arch.svg)

## Hardware

**Tested target:** [Heltec Vision Master E290](https://heltec.org/project/vision-master-e290/)
- ESP32-S3, 16 MB flash, PSRAM
- SX1262 LoRa radio
- 296×128 black-and-white e-ink (ACeP panel driven in BW mode)

Other ESP32-S3 + SX1262 + SPI display combinations would work with a new
variant directory; this repo only ships the E290 variant.

## Build & Flash

Requires [PlatformIO Core](https://docs.platformio.org) (`pip install platformio`).

```bash
git clone https://github.com/wokhouse/bap-meshcore.git
cd bap-meshcore
pio run -e bap_node                        # build
pio run -e bap_node -t upload              # flash (put device in bootloader mode first)
pio device monitor -e bap_node             # serial console, 115200 baud
```

On first boot you'll see the splash and a `bap> ` prompt. Type `help` for
the full command list.

## Provisioning

### Gateway

```
bap> role gateway
bap> wifi MyHomeWiFi "mywpa passphrase"
bap> endpoint "http://192.168.1.217:5000"     ; URL of your bap-http instance
bap> secret "tslbhBQguDbYAdYGf3H4_k829..."    ; bap-http API_SECRET
bap> keygen                                    ; generates Ed25519 keypair
                                               ; (prints the pubkey for clients)
bap> stops add 17056                           ; add stop codes to broadcast
bap> stops add 13330
bap> save
```

The gateway now connects WiFi, syncs NTP, and starts polling. Local display
shows arrivals for `stops[0]`. Mesh broadcasts one packet per stop per
poll cycle (60s default — change with `bap> poll <sec>`, min 30s).

### Client

After running `keygen` on the gateway (which printed a 64-char hex pubkey):

```
bap> role client
bap> pubkey a3f7e2c1...                       ; paste from gateway
bap> stop 17056                                ; single stop to display
bap> save
```

Client listens 100% of the time (no deep sleep in MVP). Drops any packet
that doesn't pass Ed25519 verification or doesn't match its configured stop.

## bap-http

The gateway talks to a separate service, [bap-http](https://github.com/wokhouse/bap-http),
which proxies [511.org](https://511.org)'s SIRI API and exposes a small JSON
endpoint. bap-http handles the 511.org API key, polling, and rate limits —
the gateway just consumes its cache.

See `bap-http/.env.example` for config. Default port 5000, agency SF
(SFMTA/Muni).

## Mesh Transport (how broadcasts reach clients)

We use meshcore's `PAYLOAD_TYPE_GRP_DATA` (encrypted group datagram) on a
**published** channel:

- Channel hash byte: `0x42`
- Channel secret: `SHA256("BAP_PUBLIC_CHANNEL_V1")`

Since the secret is published, meshcore's "encryption" provides no real
confidentiality — but it buys us **native multi-hop flood routing** without
patching upstream meshcore. Sender authenticity comes from a 64-byte
Ed25519 signature appended inside the encrypted payload, verified by every
client against the gateway's stored public key.

Wire format, dispatch hooks, and rationale are documented in
[docs/bap-node-plan.md](docs/bap-node-plan.md).

## Repository Layout

```
variants/heltec_e290_bap/    build target + board support (extends Heltec_E290_base)
examples/bap_node/           BAP-Node application
  main.cpp                   entry point, role branching
  MyMesh.{h,cpp}             mesh::Mesh subclass (group datagram dispatch)
  BapPacket.{h,cpp}          wire format + Ed25519 sign/verify
  BapConfig.{h,cpp}          /bap.cfg (JSON) + /bap.key (binary) persistence
  SiriParser.{h,cpp}         defensive SIRI JSON parser
  GatewayTask.{h,cpp}        WiFi + NTP + HTTP polling loop
  ArrivalDisplay.{h,cpp}     BW e-ink rendering
  BapSerialCLI.{h,cpp}       bap> command shell
docs/
  bap-node-plan.md           file-by-file implementation reference
src/, variants/, examples/   upstream meshcore (unmodified)
```

## Serial CLI Reference

Full command list (`bap> help`):

| Command | Role | Purpose |
|---|---|---|
| `role gateway\|client` | both | set device role |
| `wifi SSID "pass"` | gateway | WiFi credentials |
| `endpoint "http://host:port"` | gateway | bap-http base URL |
| `secret "token"` | gateway | bap-http bearer secret |
| `keygen` | gateway | generate Ed25519 keypair, print pubkey |
| `pubkey <64-hex>` | client | import gateway pubkey |
| `stops add\|del <code>` | gateway | manage stops to broadcast |
| `stops list` | gateway | show configured stops |
| `stop <code>` | client | single stop to display |
| `poll <sec>` | gateway | poll interval (default 60, min 30) |
| `save` | both | write `/bap.cfg` to SPIFFS |
| `show` | both | render config (secrets masked) |
| `help` | both | command list |

`wifi_pass` and `secret` are stored in `/bap.cfg` but never shown by `show`
(replaced with `***`). They can only be set, not read back.

## Configuration Files

SPIFFS partition holds three files:

| Path | Format | Contents |
|---|---|---|
| `/bap.cfg` | JSON (ArduinoJson v7) | role, wifi creds, endpoint, secret, stops, client_stop, poll_interval, pubkey |
| `/bap.key` | 64 raw bytes | Ed25519 private key (gateway only, meshcore format) |
| `/identity` | binary | meshcore node identity (auto-generated on first boot) |

`/bap.cfg` is versioned (`"version": 1`) for forward-compatibility.

## Error States

The display shows a small status line at the top when something is wrong;
last-known arrivals remain visible below.

| Status | Meaning |
|---|---|
| `WiFi DOWN` | gateway lost WiFi (auto-reconnect enabled) |
| `HTTP FAIL` | bap-http unreachable / network error |
| `HTTP 503` | bap-http returned `stale_data` (511.org poll lagging) |
| `NO PRIVKEY` | gateway has no `/bap.key` — run `keygen` |
| `NO PUBKEY` | no pubkey configured — run `keygen` (gateway) or `pubkey <hex>` (client) |
| `NO STOPS` | gateway has no stops configured — run `stops add <code>` |
| `Listening...` | client has no data yet but is configured correctly |
| `Not configured` | client has no `client_stop` set |

## Relationship to upstream meshcore

This is a fork of [meshcore-dev/MeshCore](https://github.com/meshcore-dev/MeshCore)
used as a transport layer. The upstream `src/`, most `variants/`, and most
`examples/` directories are untouched.

Differences from upstream:
- Adds `variants/heltec_e290_bap/` (build target)
- Adds `examples/bap_node/` (this application)
- Adds `docs/bap-node-plan.md`
- Modifies `README.md` (this file) — excluded from upstream merges
- Adds `bblanchon/ArduinoJson @ ^7.0.0` as a lib_dep on the `bap_node` env only

The plan doc captures the rationale for the architecture choices, especially
the group-datagram trick for native multi-hop dispatch.

## Roadmap

- [x] Gateway: WiFi → bap-http → SIRI parse → Ed25519 sign → LoRa flood
- [x] Client: receive → verify → render
- [x] Multi-hop relay (via meshcore's group datagram flood)
- [x] Serial CLI provisioning
- [ ] Live integration testing against real bap-http + 511.org
- [ ] Native unit tests for `BapPacket` encode/decode/verify
- [ ] OTA firmware updates (`esp32_ota` lib_deps already wired)
- [ ] Battery monitoring on display (`HeltecE290Board::getBattMilliVolts`)
- [ ] Deep sleep mode for client (radio wake-on-RX pattern)
- [ ] Multi-stop rotation on gateway display

## License

MIT, inherited from upstream meshcore. See [LICENSE](LICENSE).
