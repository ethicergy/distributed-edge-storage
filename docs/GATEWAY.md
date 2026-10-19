# Pi gateway on a classic ESP32 (assessment only, not implemented)

**Idea:** a fifth board, a classic ESP32 DevKit, sits between the Raspberry Pi and the cluster. It talks UART to the Pi and ESP-NOW to the four nodes. It owns no keys and is not in the ring or in any membership view. Then any of A–D can be powered off during the demo without the Pi losing the store. Today, unplugging A cuts the Pi off.

**Status:** Day 3 stretch goal, only if the core cluster and the sensors pass on hardware. Nothing here is built.

## 1. What the bridge depends on today

The Pi link is part of node A's firmware and goes through A's runtime:

| dependency | where |
|---|---|
| The bridge role is a compile-time property of node A | `src/platform/main.cpp:56` (`BRIDGE_ROLE = LOCAL_NODE_ID == config::BRIDGE_NODE_ID`), `platformio.ini:32`, `include/config.h:195-199` |
| UART1 on GPIO20/21 is opened only on A | `main.cpp:116`, `main.cpp:530-540` |
| The bridge object is built on A's `edge::Runtime` | `main.cpp:144` (`bridge::Bridge pi_bridge(runtime, …)`), `include/bridge.h:48` |
| UART bytes are fed only in A's node task | `main.cpp:426-440` |
| Every Pi request becomes a client op of A: `runtime_.submit(…)` with origin = A | `src/bridge.cpp:204`; identity rule in `include/bridge.h:23-26` |
| Ping answer and time calibration use A's id and HLC | `bridge.cpp:98-105`, `bridge.cpp:144` |
| `submit` refuses until the node is ACTIVE | `src/node_runtime.cpp:1217-1218` → `NOT_RESPONSIBLE` to the Pi (`bridge.h:28-31`) |

So the Pi is cut off when A is off, and also for about 5.5 s after A reboots (BOOTING + CATCHUP) and while A's client-op slots are busy (`MAX_CLIENT_OPS = 8`, `config.h:94`; bridge in-flight 4, `config.h:199`).

The protocol-level part of the bridge does not care which node runs it: it takes a protocol envelope, submits a client op, and answers with the Pi's request id. That is what the design reuses.

## 2. Smallest design: forward to a live member, which proxies

```
Pi ──UART──► gateway (ESP32) ──ESP-NOW unicast──► member M (any of A–D, ACTIVE)
                                                     │ Bridge::on_envelope → runtime.submit (origin = M)
Pi ◄─UART── gateway ◄──ESP-NOW unicast── reply ──────┘ (Pi's request id echoed, M's id + HLC in header)
```

* **The gateway is a relay, not a client.** It decodes the Pi's UART frames (`AA 55 | len | envelope | CRC`), sends the bare envelope (≤ 250 B, `protocol.h:15`, which fits one ESP-NOW frame) to a chosen member, and wraps the member's reply envelope back into a UART frame for the Pi. It never builds cluster packets, has no node id in the protocol, no HLC, no dedup.
* **The member executes the request as its own client request,** exactly as node A does today. A second `bridge::Bridge` instance on every member serves gateway traffic, so routing, replication, retries, dedup and versioning are unchanged. The Pi's `ping` now answers `bridge is node B` (or whichever member answered), and time calibration uses that member's HLC.
* **Picking a member.** The gateway listens to the members' broadcast heartbeats, which ESP-NOW receives without a peer entry. Members send heartbeats only while ACTIVE (`node_runtime.cpp:1748-1752`). The gateway keeps `last_heard[4]` and treats a member as usable if it was heard within `MEMBERSHIP_TIMEOUT_MS` (2 s, `config.h:25`). That is the same rule the members apply to each other. It sends each request to the lowest-id usable member and sticks with it while it stays usable. Sticking keeps one member's client slots warm and makes logs easy to follow.
* **Failover.**
  1. **Before acceptance:** the ESP-NOW send status reports no MAC-level ACK (the member is off). Mark it unusable and resend the same envelope to the next usable member immediately. This costs milliseconds.
  2. **During a request:** the member accepted it, then died. Nothing comes back. The member's heartbeats stop, so after ≤ 2 s it is unusable. The gateway does not guess whether the request ran. The Pi's own retry (`--timeout`, default 10 s, same request id) goes to the next member.
  * Lowering the Pi's `--timeout` to about 4 s shortens the gap. A client attempt is bounded by `CLIENT_ATTEMPT_DEADLINE_MS` = 3 s plus backoff (`config.h:50-59`), so a slow but healthy request can still take longer than that.
* **What the Pi sees:** an unchanged CLI and wire format on `/dev/serial0` (now wired to the gateway's UART2). A request in flight when its member dies is answered by another member after the Pi's retry.
* **Rejected alternative:** the gateway as a fifth protocol node (origin id 5) sending requests straight to primaries. That touches the frozen id range, every array sized `node::MAX` and the MAC tables (section 3 shows where), plus placement must keep it out of the ring. Far bigger, for no demo benefit.

## 3. Changes on the members

| topic | today | change needed |
|---|---|---|
| Unknown MAC dropped | RX callback drops any MAC outside the table and counts it (`src/platform/transport.cpp:127-131`); `on_send` ignores unknown MACs (`transport.cpp:167-171`) | Add `MAC_GATEWAY` (one more table entry), add it as an ESP-NOW peer in `begin()` (`transport.cpp:193-301`, next to the member peers at `:243-275`), mark its frames with a sentinel source `node::GATEWAY` (e.g. `0xFE`, not a protocol id, not `BROADCAST 0xFF`, `include/node_id.h:19`) instead of dropping them, and add `send_to_gateway(data, len)`. Per-peer TX stats arrays are sized for 4 nodes (`transport.cpp:53-54`, `:421-431`): count gateway sends separately. |
| Dispatch | Node task passes every radio frame to `runtime.on_frame` (`main.cpp:414-416`) | `if (frame.source_node == node::GATEWAY) gateway_bridge.on_envelope(…) else runtime.on_frame(…)`. The gateway's frames never reach the runtime. |
| Bridge | Envelopes enter only through the UART decoder (`bridge.cpp:50-75`, `handle_envelope` is private, `bridge.h:68`), and replies are always UART-framed (`bridge.cpp:77-86`) | Public `on_envelope(data, len, now)`; a constructor flag for unframed output (ESP-NOW has its own CRC; a framed max-size reply would be 254 B > 250). A second instance `gateway_bridge(runtime, write_to_gateway, …)` on every node. |
| Node id range | Protocol ids must be 1–4 (`src/protocol.cpp:5-8`, enforced on origin/sender at `protocol.cpp:994`, `:1053`); arrays sized by `node::MAX` (`include/membership.h:10-11`, `node_runtime.cpp:389`) | **No change.** The gateway never appears in a protocol header the runtime parses. The Pi's header ids must be valid but are ignored (`bridge.h:26`); the member re-originates. |
| Dedup keyed by node | Identity = (origin, request_id) (`include/request_identity.h:9-11`), checked on the primary (`node_runtime.cpp:436-478`) | **No change:** origin is the proxying member with its own request ids. Caveat, same as today's bridge: a Pi retry after a member died mid-request is a new cluster request and may apply a SET/DEL twice. Both are idempotent (`STATUS.md` §8). |
| HLC origin | Versions are assigned by the primary (`node_runtime.cpp:545-551`); every packet's HLC is merged (`node_runtime.cpp:381-384`) | **No change:** the gateway sends no cluster packets, and replies carry the member's HLC for the Pi's calibration. |
| Membership / heartbeats | Heartbeats feed membership in `on_frame` (`node_runtime.cpp:386-390`) | **No change.** The gateway sends no heartbeats and its frames bypass `on_frame`, so it can never appear in a view or the ring. |
| Node A's UART bridge | `main.cpp:426-440` | Keep it. It is the fallback if the gateway fails, and it's one cable swap on the Pi. |
| Capacity | Gateway requests share the member's 8 client-op slots with the sampler and console | Fine for one Pi; `BRIDGE_MAX_INFLIGHT` (4) applies per bridge instance. |

Put all member changes behind a compile flag (e.g. `-DEDGE_GATEWAY=1` in `[env]`). Images built without it stay byte-identical to the validated firmware.

## 4. Gateway firmware

* **New env:**

  ```ini
  [env:gateway]
  board = esp32dev
  build_src_filter = +<*> -<platform/main.cpp> -<platform/sensors.cpp> -<platform/eeprom.cpp> +<../gateway/>
  build_flags = ${env.build_flags} -DEDGE_GATEWAY=1
  ```

  * It reuses `platform = espressif32@7.1.3`: Arduino-ESP32 2.0.17 / IDF 4.4 supports the classic ESP32, and the ESP-NOW callback signatures in `transport.cpp:14-16` are the same.
  * Override `ARDUINO_USB_MODE` and `ARDUINO_USB_CDC_ON_BOOT`: they are C3-only. Set `build_unflags` for them, or move them from `[env]` into the node envs.
  * The DHT `lib_deps` are not needed.
* **Gateway main** (`gateway/main.cpp`, about 250 lines): reuses `uart_frame` (decoder/encoder) and `protocol` (header parse for heartbeat sniffing) from `src/`, and its own small ESP-NOW wrapper with the four member MACs as peers on channel 6.
* **Pins:**
  * UART2 to the Pi on **GPIO16 (RX2) / GPIO17 (TX2)**: the defaults. On a WROVER module 16/17 belong to PSRAM, so use 25/26.
  * Avoid strapping pins 0, 2, 5, 12, 15. GPIO12 high at boot selects 1.8 V flash.
  * UART0 (GPIO1/3) is the USB console through the board's CP2102/CH340, which shows up as `/dev/ttyUSB*`, not `ttyACM*`.
* **Chip notes:**
  * Classic ESP32 is dual-core: Wi-Fi on core 0, Arduino `loop` on core 1. The RX callback must still only queue (same rule R3).
  * Unlike the C3, its USB console is a separate UART chip, so console output never blocks on a missing USB host.
  * Its RF is better than the SuperMini's, so it can use the same 8.5 dBm or more.
  * Its MAC goes into the members' `MAC_GATEWAY`; `scripts/read_macs.sh` needs `--chip esp32` and `/dev/ttyUSB*` for it.
* **Host coverage:** put member selection, liveness, and resend-on-MAC-failure in a portable `gateway_router.{h,cpp}` under `src/`, with unit tests. Extend `sim::Cluster` with a gateway endpoint so `cluster_test` can run "A off while the Pi keeps reading through the gateway".

## 5. Estimate and riskiest part

| work | h |
|---|---|
| `Bridge::on_envelope` + unframed mode + host tests | 1.5 |
| Member transport/dispatch changes behind `EDGE_GATEWAY` | 1 |
| `gateway_router` (liveness, choice, resend) + unit tests | 1.5 |
| Gateway main, env, build | 2.5 |
| Sim endpoint + cluster scenario (gateway with A off) | 1.5 |
| Hardware: MACs, flash 5 boards, Pi rewire, failover tests | 2 |
| **total** | **10** |

That is more than a stretch slot on Day 3. A cut-down version without the sim scenario and with a fixed member preference is about 7 h. It is still only realistic if Day 2 ends with everything green.

**Riskiest part:** reflashing the four validated members on demo day. The member changes are small, but they touch the transport RX path every frame goes through, and they invalidate the hardware test logs taken on the old images. Mitigation: the `EDGE_GATEWAY` flag, keeping node A's UART bridge as an instant fallback, and re-running pre-flight + H1/H2/H14 after flashing. Second risk: failover timing seen by the Pi. A member that dies mid-request costs a full Pi `--timeout` before the retry; rehearse with `--timeout 4`.
