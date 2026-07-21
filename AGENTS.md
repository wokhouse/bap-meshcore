# AGENTS.md

Guidance for AI coding agents (and humans pair-programming with them) working on
**bap-meshcore**. Read this before making changes.

> **Where to open PRs — read this first.**
>
> This is a **fork** of [meshcore-dev/MeshCore](https://github.com/meshcore-dev/MeshCore),
> used purely as a LoRa mesh transport layer. The BAP-Node application lives only
> in this fork.
>
> **Open all pull requests against [`wokhouse/bap-meshcore`](https://github.com/wokhouse/bap-meshcore),
> NOT the upstream `meshcore-dev/MeshCore` repo.** Upstream maintainers will close
> BAP-related PRs; they do not own this code. The default branch is `main`.
>
> The upstream `CONTRIBUTING.md` (which references the `dev` branch, the
> `🤖🤖` agent PR flag, etc.) applies to **upstream meshcore** and is kept for
> merge hygiene. It does **not** describe this project's workflow.

## What this project is

Firmware for displaying transit arrival times on a Heltec Vision Master E290
e-ink display, broadcast over a LoRa mesh. It is intended for **worldwide use**:
any transit agency can be supported by pointing the gateway at a compatible
[bap-http](https://github.com/wokhouse/bap-http) instance. Agencies whose API is
compatible with the [511.org](https://511.org) SIRI API (SFMTA/Muni and other
Bay Area agencies) work out of the box; elsewhere, bap-http is the integration
point — the node firmware itself is agency-agnostic and only consumes the JSON
that bap-http serves.

### Architecture (three components)

```
transit agency API ── bap-http ── gateway node ──LoRa mesh── client node(s)
                       ▲            (this repo)                 (this repo)
                       │
              agency abstraction layer
              https://github.com/wokhouse/bap-http
```

- **[bap-http](https://github.com/wokhouse/bap-http)** (separate repo) — the
  abstraction layer between a transit agency and the mesh node. It handles the
  agency-specific API key, polling, rate limits, and SIRI translation, then
  exposes a small JSON endpoint. This is where you add support for a new agency
  or a non-SIRI feed — **not** in the firmware.
- **bap-meshcore** (this repo) — the LoRa node firmware. One binary, two
  runtime roles selected at provisioning time:
  - **Gateway** — connects WiFi, polls its configured bap-http instance every
    60s, parses the SIRI-style JSON, signs each packet with Ed25519, floods
    over LoRa.
  - **Client** — listens on LoRa, verifies signatures, renders arrivals. No
    WiFi, no credentials, no upstream dependency.

See `README.md` for the user-facing overview and `docs/bap-node-plan.md` for the
file-by-file implementation rationale (especially the group-datagram trick that
gets multi-hop flood routing without patching upstream meshcore).

## Repository layout (what's ours, what's upstream)

```
variants/heltec_e290_bap/    OUR build target + board support (extends Heltec_E290_base)
examples/bap_node/           OUR BAP-Node application
docs/bap-node-plan.md        OUR implementation reference
src/, most variants/,        UPSTREAM meshcore — DO NOT MODIFY unlessabsolutely necessary
  most examples/             (changes here break future upstream merges)
README.md                    OURS (excluded from upstream merges)
CONTRIBUTING.md              UPSTREAM (do not rewrite for this fork)
```

**Treat `src/`, upstream `variants/`, and upstream `examples/` as read-only.**
If you think a fix needs to touch upstream code, surface it as an issue first —
the right answer is usually an application-side workaround that keeps the merge
surface clean.

## Build, test, flash

Requires [PlatformIO Core](https://docs.platformio.org) (`pip install platformio`).

```bash
pio run -e bap_node                          # build
pio run -e bap_node -t upload                # flash (put device in bootloader mode first)
pio device monitor -e bap_node               # serial console, 115200 baud
pio test --environment native --verbose      # run unit tests (upstream)
```

There is only **one** BAP build target: `bap_node` (defined in
`variants/heltec_e290_bap/platformio.ini`). There is no separate gateway/client
target — the role is selected at runtime via the serial CLI (`role gateway` /
`role client`), then `save`.

### Flashing hardware (Heltec E290)

The board must be in **bootloader mode** before `pio run -t upload` will find it:
hold **BOOT**, tap **RST**, release **BOOT**. The USB-CDC port then enumerates
with a MAC-embedded name like `/dev/cu.usbmodem1020BA3BBAC81`; the runtime app
enumerates as `/dev/cu.usbmodem101` (macOS). On Linux the names differ but the
MAC-embedded-vs-generic distinction holds.

After upload, esptool hard-resets into run mode automatically. Verify success by
confirming `Hash of data verified` in the esptool output and that the port
re-enumerates back to the runtime name.

## Architecture notes & hard-won gotchas

These are subtle traps we have already hit. Read before touching the relevant
code, and don't reintroduce them.

### Group-datagram framing determines the packet budget

`BAP_DATA_BUDGET` (in `examples/bap_node/BapPacket.h`) is **not** simply
`MAX_PACKET_PAYLOAD - signature`. The signed blob is wrapped by meshcore's
group-datagram layer (channel hash byte + AES-128 block padding) before it hits
the `MAX_PACKET_PAYLOAD` cap. The constant is derived from the framing constants
(`MAX_PACKET_PAYLOAD - PATH_HASH_SIZE - (CIPHER_BLOCK_SIZE-1) - BAP_SIG_LEN`),
currently **104 bytes**. If you change `BAP_DEST_MAX`, `BAP_MAX_VISITS`, or any
string length, re-derive this and confirm a full packet still fits.

### Ed25519 signature length must be reconstructed on RX

meshcore's group-datagram cipher (`Utils::encrypt`/`Utils::decrypt`) rounds the
plaintext up to a 16-byte AES block boundary, and `decrypt` returns the padded
length. The BAP signature is made over the **unpadded** plaintext, so the
verifier cannot use `len - 64` as the signed length. Use
`BapPacket::verifySigned(buf, buf_len, pubkey)`, which walks the packet
structure (header + null-terminated strings + ETA bytes) to recover the exact
signed length. `decode()` relies on the same walk to locate the signature.

### NTP is async — gate the first poll on it

`configTime()` returns immediately; `time(nullptr)` reads 0 for several seconds
until SNTP resolves. The gateway must not poll before NTP is actually synced, or
ETAs come out as the 254-minute clamp. `GatewayTask::loop()` gates on
`time(nullptr) >= NTP_SANITY_EPOCH` and re-arms the gate on WiFi disconnect.

### Serial capture from non-interactive processes

The ESP32-S3 native USB-CDC interface does not reliably emit to a non-TTY serial
open (e.g. a backgrounded `pyserial` read). For automated RX/broadcast capture,
prefer a real terminal (`pio device monitor`, `cu`, `screen`) with a human in
the loop, or add temporary `Serial.printf` instrumentation to the firmware and
read the logs interactively. Remove all such instrumentation before committing.

### Ed25519 key format (meshcore convention)

meshcore's `LocalIdentity` stores a 64-byte "private key" that is the expanded
form produced by `ed25519_create_keypair` (not a raw seed, not seed+pub).
`ed25519_derive_pub(pub, prv)` reproduces the 32-byte public key from it.
`BapPacket::sign` derives the pubkey from the privkey at sign time, and clients
verify against the pubkey pasted in via the serial CLI. `BAP_PRIVKEY_LEN` is 64.

## Coding conventions

Follow the existing style in the file you're editing — consistency with
surrounding code beats any rule.

- 2-space indentation, no tabs (matches `.clang-format`).
- `camelCase` for functions/variables, `PascalCase` for classes, `ALL_CAPS` for
  `#define` constants.
- Keep lines under ~100 chars where reasonable.
- **No dynamic memory allocation** except during `setup`/`begin` (embedded
  constraint; matches upstream). The one deliberate `new` in `main.cpp` is for
  the mesh packet pool, sized once at boot.
- Do **not** retroactively reformat existing code — it creates noise that hides
  real changes in diffs.

## Commit & PR conventions

- **All PRs target [`wokhouse/bap-meshcore`](https://github.com/wokhouse/bap-meshcore),
  branch `main`.** Never open BAP PRs against `meshcore-dev/MeshCore`.
- Commit messages: imperative mood, first line ≤ ~72 chars, body explains *why*.
  Reference issues (`Fixes #N`) where applicable.
- One logical change per commit; one feature/fix per PR.
- If a change is non-obvious (a budget recalculation, a signature-length walk,
  an NTP gate), explain the reasoning in the commit body or a code comment.
  Future readers — human and agent — will thank you.
- Keep `README.md` and `docs/bap-node-plan.md` in sync with user-visible
  behavior changes.

## Before you finish a task

- Build passes: `pio run -e bap_node`
- No debug/temporary `Serial.printf` instrumentation left behind (search the diff
  for `DEBUG`, `BAP-RX:`, etc.)
- Upstream `src/` and unrelated `variants/`/`examples/` untouched unless the
  change is explicitly upstream-scoped (and if so, it belongs upstream, not here)
- If you flashed hardware for testing, note the final state of the boards
