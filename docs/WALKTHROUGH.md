# Code walkthrough (for the demo and viva)

A guided tour of edge-kv through four questions: how a write travels, how a sensor reading reaches the Pi, what a power failure does, and which distributed-systems ideas are used where. The examiner questions are at the end.

File:line references are to branch `hw-bringup`. Three facts make the rest easy to follow:

* **Every node runs the same code.** Only `NODE_ID` differs (`platformio.ini`). All behaviour comes from what each node computes from its own view of who is alive.
* **One task does everything.** The node task in `src/platform/main.cpp:402-449` loops forever and calls `runtime.on_frame` for received radio frames, the console, the Pi bridge (on A), `runtime.tick`, `sampler.tick` and `console.tick`. The ESP-NOW receive callback only copies frames into a queue (`src/platform/transport.cpp:114-157`). So the distributed-store state needs no locks.
* **Everything outside `src/platform/` is plain C++17.** The same code runs four times inside one PC process in the simulator (`sim/cluster.cpp`). That is how every scenario below is tested (`test/cluster_test.cpp`).

---

## 1. The life of `set k v` typed on node B

The ring puts key `k` on **primary D, replica B** (`ring k` on any console). So this one command touches B → D → B → D → B.

### On B: the console and the client side

| # | what happens | where |
|---|---|---|
| 1 | Characters arrive over USB. Enter dispatches the line. | `main.cpp:426-428` → `Console::on_char` (`src/console.cpp:132`) → `Console::execute` (`console.cpp:163`, `"set"` at `:179`) |
| 2 | `cmd_kv` splits key and value and calls the runtime's client API. | `console.cpp:385-415` → `Runtime::submit` (`src/node_runtime.cpp:1240`) |
| 3 | `submit` checks the node is ACTIVE and the sizes (key ≤ 32 B, value ≤ 192 B). It copies the operation into a free **client-op slot** (8 of them), phase WAITING. Nothing is sent yet. | `node_runtime.cpp:1252-1300` |
| 4 | On the next `runtime.tick` → `tick_client_ops` → `start_attempt`. | `node_runtime.cpp:1691`, `:1623`, `:1329` |
| 5 | **Placement:** `placement_.locate(key, view)` hashes the key with FNV-1a and walks the ring of 4 × 64 virtual points clockwise from that hash. The first live node is the primary (D), the next different live node is the replica (B). `view` is B's own set of live nodes. | `node_runtime.cpp:1347`; `src/placement.cpp:55-150`; `src/consistent_hash.cpp` (FNV-1a); `src/hash_ring.cpp:58` (ring built once at boot) |
| 6 | A new **request identity** `(origin = B, request_id)` and an **HLC timestamp** for the packet. | `ids_.next()` (`src/request_identity.cpp:59`); `clock_.on_local_event()` (`src/hlc.cpp:106`); packet built by `packet::build_request` (`src/packet_builder.cpp:49`) into the frozen 22-byte header format (`include/protocol.h`) |
| 7 | Target D ≠ B, so the op becomes REMOTE. The **request manager** takes the packet and sends it, then keeps a copy for retransmission. | `client_requests_.submit` (`src/request_manager.cpp:93`) → `Runtime::reliable_send` (`node_runtime.cpp:1164`) → `send` (`:1113`) → `EspNowLink::unicast` (`main.cpp:74`) → `EspNowTransport::send_unicast` (`transport.cpp:307`) → `esp_now_send` to D's MAC |
| 8 | If no answer arrives in 600 ms, it retransmits the same packet with `FLAG_RETRY`, backing off up to 1200 ms, at most 3 times. The attempt deadline is 3 s. | `request_manager.cpp:201-283`; timings `include/config.h:47-59` |

### On D: the primary

| # | what happens | where |
|---|---|---|
| 9 | The ESP-NOW callback maps the sender MAC to node B and queues the frame. The node task hands it to the runtime. | `transport.cpp:114-157`; `main.cpp:420-422` |
| 10 | `on_frame` validates the header, rejects a frame whose header sender differs from the radio sender (spoof check), and **merges B's HLC** into D's clock. | `node_runtime.cpp:336-385` |
| 11 | SET_REQ → `handle_client_request`. If D isn't ACTIVE it answers `NOT_READY` (boot gate). | dispatch `node_runtime.cpp:394-397`; gate `:454-467` |
| 12 | **Dedup:** `begin_request((B, id))`. A new request is marked IN_PROGRESS. If it's a duplicate, the cached reply is replayed instead of running the request again (this is the exactly-once part). | `node_runtime.cpp:473-535`; `src/dedup_manager.cpp:137` |
| 13 | **Version:** `version = (clock_.on_local_event(), D)`. D's clock has already merged B's, so the version is newer than anything either has seen. | `node_runtime.cpp:546-552` |
| 14 | `engine_.execute` checks that D really is the primary in **D's own view**. If not, it answers `NOT_RESPONSIBLE`. Then `execute_set` → `storage_.set` → `apply`. | `src/kv_engine.cpp:71-144`, `:231`; `src/storage.cpp:1528`, `:1411` |
| 15 | **Last-writer-wins check:** `compare_versions(new, stored)` compares the HLC first, then the origin id. Not newer → `STALE_VERSION`. | `storage.cpp:1462-1475`, `compare_versions` at `storage.cpp:85` |
| 16 | **Append to EEPROM:** a record (header, key, value, CRC-16) is written **without its commit byte**, read back and verified, then the commit byte `0xA5` is written last. A power cut before that byte leaves a record that recovery ignores. The RAM index (64 slots) is updated. | `append_record_to_bank` (`storage.cpp:866`; body `:959`, verify `:971`, commit `:996`); layout `include/storage_record.h`; driver `src/platform/eeprom.cpp:163` (64-byte pages, ACK polling) |
| 17 | The local change is noted for anti-entropy. Because the response names replica B, D starts **replication**: a REPL_REQ carrying the version (`hlc` + origin D) and `FLAG_REPLICATED`, through its own request manager (300 ms retries, 1.5 s deadline). | `node_runtime.cpp:575-587` → `begin_replication` (`:658-748`) |

### On B again: the replica

| # | what happens | where |
|---|---|---|
| 18 | REPL_REQ → `handle_replication_request`. It goes through dedup as well, so a retransmitted REPL_REQ is applied once. | `node_runtime.cpp:884-931` |
| 19 | `engine_.apply_versioned` stores the value **with D's version**, using the same LWW `storage::apply` as on D. | `node_runtime.cpp:938`; `kv_engine.cpp:146-229` |
| 20 | It builds a REPL_ACK, caches it in dedup, and sends it to D. | `node_runtime.cpp:966-1000` |

### On D: completing the write

| # | what happens | where |
|---|---|---|
| 21 | REPL_ACK → `handle_replication_ack` → `finish_replication(OK)`. | `node_runtime.cpp:750-820`, `:822` |
| 22 | `respond_to_client` builds the SET_ACK, **caches it in dedup** under `(B, id)` so a retry gets the same answer, and delivers it to the origin, B. | `node_runtime.cpp:602-651`; builder `src/kv_response.cpp:159` |
| 23 | If B (as replica) had not answered within 1.5 s, D would answer `REPLICATION_FAILED`. The write stays committed on D, and anti-entropy copies it later (decision D5). | `node_runtime.cpp:839-882` |

### On B: the reply

| # | what happens | where |
|---|---|---|
| 24 | SET_ACK → `handle_client_response`: it matches `(B, id)` to the client-op slot and stops the retransmissions. | `node_runtime.cpp:1513-1566` |
| 25 | `on_op_response` → `finish_op` fills a `ClientResult`: status, served by D, attempts, latency. | `node_runtime.cpp:1459`, `:1569` |
| 26 | At the end of the tick, `deliver_completions` frees the slot and calls the console's completion callback, which prints the line. | `node_runtime.cpp:1666-1688` → `Console::on_complete`/`handle_result` (`console.cpp:527-576`) |

```
SET k: OK (served by D, 1 attempt(s), 11 ms)
```

**Variations worth knowing:**
* If B itself were the primary, step 7 takes the **local fast path**: the same server code runs without the radio (`node_runtime.cpp:1386-1400`).
* `get` is the same trip without replication. GET is not deduplicated because it is idempotent (`node_runtime.cpp:468-476`).
* `del` writes a **tombstone** instead of a value.

---

## 2. A sensor reading on its way to the Pi

Example: node D's temperature (DHT11). The key `s/D/temp` lives on **primary B, replica D**.

### Measuring and storing (on D)

| # | what happens | where |
|---|---|---|
| 1 | A separate low-priority task reads the DHT11 every 2 s and records ok/failed and the last good value under a spinlock. Only when D's bit in `REAL_SENSOR_NODES` is set; otherwise `SimulatedSource` provides values. | `src/platform/sensors.cpp:79-102` (task), `:64` (`take`); switch `include/config.h:151`; `main.cpp:101-118` |
| 2 | Every node-task iteration calls `sampler.tick`. Every 30 s (`SENSOR_PERIOD_MS`), staggered per node, it reads the source. | `main.cpp:448` → `Sampler::tick` (`src/sensor.cpp:398`) |
| 3 | `FallbackSource::read` → `LatestValueSource::read` takes the DHT task's results. It keeps the last good value, and after 10 s without a good read flags it `STALE` (or `ERROR` if there was never one). It is never replaced by simulated data. | `src/sensor_sources.cpp:225`, `:160` |
| 4 | The sample is encoded as a 20-byte reading: kind, flags, sequence, value in thousandths, HLC of the sample. Event kinds (motion, sound) use 24 bytes. | `sensor.cpp:87` (`encode`); format comment `include/sensor.h:51-81` |
| 5 | Two ordinary client SETs: `s/D/temp` (latest) and `h/D/temp/<seq % 4>` (4-slot history ring). From here it is **exactly the trip in part 1**: primary B stores and replicates to D. | `sensor.cpp:485-522` → `Runtime::submit` |
| 6 | After a reboot, the sampler first reads its own latest key back from the cluster, so sequence numbers continue. | `sensor.cpp:411-436`, `Sampler::handle` `:365` |

### Reading it on the Pi (`python3 -m edgekv sensors`)

| # | what happens | where |
|---|---|---|
| 7 | The CLI asks the client for each node's latest key, e.g. `GET s/D/temp`. | `pi/edgekv/cli.py` `cmd_sensors` → `client.py:171` → `sensor` `:167` → `get` `:151` |
| 8 | `_transact` builds a GET_REQ packet with a Pi-chosen request id, frames it `AA 55 \| len \| packet \| CRC-8`, and writes it to the UART. | `client.py:93-133`; `pi/edgekv/framing.py` |
| 9 | On node A, the node task reads UART1 (GPIO20) and feeds the bridge. The frame decoder resynchronises on noise such as boot text. | `main.cpp:430-444` → `Bridge::on_bytes` (`src/bridge.cpp:50`); decoder `src/uart_frame.cpp` |
| 10 | `handle_envelope` turns the Pi's packet into an ordinary **client op of node A** (`runtime_.submit`) and remembers the Pi's request id. | `bridge.cpp:126-223` (submit at `:204`) |
| 11 | A's runtime sends GET_REQ to B (primary of `s/D/temp`). B answers GET_RESP with the 20 bytes. This is part 1's path without replication. | as in part 1 |
| 12 | The completion goes to `Bridge::on_complete`, which answers the Pi with **the Pi's request id** and A's current HLC in the header. | `bridge.cpp:225`, `respond` `:88`, `send_envelope` `:77` → `bridge_uart.write` (`main.cpp:147`) |
| 13 | On the Pi: the frame is decoded and matched to the request id. The bridge's HLC calibrates `offset = pi_wall_ms − hlc_ms`. | `client.py:121-124`, `_calibrate` `:88` |
| 14 | `reading.decode` unpacks the 20 bytes. `wall_time(hlc)` turns the sample's HLC into Pi wall-clock time. The CLI prints it, and `watch --csv` appends a CSV row. | `pi/edgekv/reading.py:92`; `client.py:183`; `cli.py:45` (`_format_reading`), `:55` (`_csv_row`) |

---

## 3. Node C loses power and comes back

### While C is off

| t | what happens | where |
|---|---|---|
| 0 | C stops sending heartbeats. Every node normally broadcasts one every 500 ms while ACTIVE. | `node_runtime.cpp:1783-1807`; `config.h:22` |
| 0–2 s | Requests and replications sent to C go unanswered and are retransmitted. | `request_manager.cpp:201` |
| ~2 s | On every other node `membership.is_alive(C)` turns false (2 s without a heartbeat). `tick_view` logs `view: AB-D`. | `src/membership.cpp:85-121`; `node_runtime.cpp:1759-1781`; `config.h:25` |
| after | **Placement changes on its own:** a key whose primary was C now maps to the next live node on the ring. Only C's keys move. A client attempt that timed out is retried (up to 5 attempts, 400 ms × n backoff) with placement recomputed, so it reaches the new owner. | `node_runtime.cpp:1422-1455` (`attempt_failed`), `:1347` (`locate` per attempt) |
| after | Writes whose replica is C: the primary times out after 1.5 s and answers `REPLICATION_FAILED`, which the client sees as `UNDER_REPLICATED`. The write is committed and will be repaired. | `node_runtime.cpp:839-882`; decision D5 |
| after | Anti-entropy rounds (every 10 s) also **hand off** existing copies to the new owners. | `node_runtime.cpp:1859`; `src/anti_entropy.cpp:227-347` |

### When C powers back on

| step | what happens | where |
|---|---|---|
| 1 | **Storage recovery:** read both superblock copies and pick the valid, newer bank. Scan its records and rebuild the RAM index from committed records. A torn last record (power cut mid-write) is discarded and wiped. | `storage.cpp:1702` (`begin`), `:598` (`recover_active_bank`) |
| 2 | **Clock rule:** the HLC starts at or above the highest version stored, so C never issues an older version than one it already holds. | `node_runtime.cpp:271-276`; `hlc.cpp:23` |
| 3 | **BOOTING (2.5 s):** C listens and sends no heartbeats, so no one routes requests to it. Its clock merges everyone else's. | `tick_state` `node_runtime.cpp:1708-1731`; heartbeats only when ACTIVE `:1785` |
| 4 | **CATCHUP (3 s):** peers were heard, so C broadcasts its **version table** twice (key + version per entry, in chunks). Client requests sent to it get `NOT_READY`. | `node_runtime.cpp:1735-1752`; `anti_entropy.cpp:71` (`start_round`); gate `node_runtime.cpp:454` |
| 5 | Each peer compares C's table with its own. For every key C owns where the peer is newer, or that C lacks entirely (complete table), it queues a **push**: a RESYNC_DATA packet carrying the value and its original version. | `anti_entropy.cpp:227-355`; `node_runtime.cpp:1810-1857` (`send_push`) |
| 6 | C applies each push through the same LWW `storage::apply` and logs `anti-entropy: key k6 repaired from A`. Tombstones are pushed the same way, so a key deleted while C was off stays deleted. | `node_runtime.cpp:1003-1048` (`handle_resync`) → `kv_engine.cpp:146` |
| 7 | **ACTIVE:** C heartbeats again. Within ~1 s every view is `ABCD`, placement maps C's keys back to C, and C serves them, already up to date. Test H7H8 checks that a stale value is never returned during this. | `node_runtime.cpp:308-325`; `test/cluster_test.cpp` `H7H8` |
| 8 | **Cleanup:** the nodes that stood in for C now hold **orphan copies**. Garbage collection drops them once both current owners report a version at least as new. Tombstones are dropped after 10 min, once all four nodes confirm. | `anti_entropy.cpp:397-479`; decision D8 |

If C's EEPROM was wiped or corrupted, the same steps rebuild it from scratch (`STORAGE_AUTO_FORMAT_ON_CORRUPTION`, `node_runtime.cpp:233-243`; test `H11`).

---

## 4. Cheat sheet: concepts and where they live

| concept | in two sentences | code |
|---|---|---|
| **Consistent hashing** | Keys and nodes are placed on the same hash ring (64 virtual points per node); a key belongs to the first live node clockwise, and its replica to the next different node. When a node fails, only its keys move, unlike `hash mod N`, which would move most keys. | `src/consistent_hash.cpp` (FNV-1a), `src/hash_ring.cpp`, `src/placement.cpp:55-150`; `ring <k>` on the console |
| **Replication (RF = 2)** | The primary stores a write, then sends REPL_REQ to the replica and answers the client after the replica's ACK. If the replica is down, the write stays committed on the primary as `UNDER_REPLICATED` and is repaired later. | `node_runtime.cpp:658-882` (primary), `:884-1001` (replica); decision D5 |
| **Hybrid logical clock (HLC)** | A 64-bit timestamp: 48 bits of milliseconds plus a 16-bit counter. Every packet carries it and every receiver merges it. Versions then follow causal order and stay close to real time, even though the boards have no wall clock. | `src/hlc.cpp` (`on_local_event` `:106`, `on_receive` `:154`, `adopt_physical` `:256`); merged in `node_runtime.cpp:382-385` |
| **Last-writer-wins (LWW)** | Every stored value carries a version (HLC, origin node). A write or repair is applied only if its version is strictly newer, with ties broken by node id, so all copies converge to the same value without coordination. | `compare_versions` `storage.cpp:85`, check in `storage::apply` `:1462-1475` |
| **Tombstones** | A delete is stored as a versioned "deleted" record rather than erased, so an older copy elsewhere cannot bring the key back. They are garbage-collected after 10 min once every node has confirmed them. | `State::TOMBSTONE` in `include/storage.h`; `FLAG_TOMBSTONE` in replication/resync; GC `anti_entropy.cpp:397`; test `H9` |
| **Dedup / exactly-once** | Each request has an identity (origin node, request id) and retries reuse it. The server remembers recent identities with their cached reply, so a retransmitted or duplicated request is answered from the cache instead of being executed twice. | `src/dedup_manager.cpp`; used at `node_runtime.cpp:473-535` (client) and `:900-933` (replica); retries in `src/request_manager.cpp`; tests `H4`, `H5` |
| **Anti-entropy** | Every 10 s, and during catch-up, each node broadcasts a table of its keys and versions, and whoever holds a newer copy of a key the sender owns pushes it. This one mechanism repairs under-replicated writes, catches up rebooted nodes, hands keys off after placement changes, and rebuilds a wiped node. | `src/anti_entropy.cpp`; `node_runtime.cpp:1810-1924`, `:1003-1063`; tests `H6`, `H11` |
| Membership / failure detection | Heartbeat every 500 ms; a peer is dead after 2 s of silence. Each node keeps its own view, with no leader and no agreement round. | `src/membership.cpp`, `node_runtime.cpp:1783`; `config.h:22-25` |
| Boot gate | A rebooted node stays silent, then catches up, then serves. This prevents stale reads. | `node_runtime.cpp:1708-1757`; decision D6 |
| Crash-safe storage | Append-only log in two banks with CRC and a commit byte written last; compaction switches banks atomically through a superblock. | `src/storage.cpp`; `test/storage_manager_test.cpp` cuts power at every byte |

---

## 5. Likely examiner questions

Questions 1–10 are about the distributed-systems design, 11–22 about the hardware and engineering.

### Design

1. **Why consistent hashing instead of `hash(key) % 4`?**
   With modulo, removing one of four nodes remaps about 3/4 of all keys. With the ring, only the failed node's keys move, to the next node clockwise. The 64 virtual points per node spread the load evenly (23–27 % each, `test/ring_balance_test.cpp`).

2. **Is the store strongly consistent?**
   No. It is eventually consistent with last-writer-wins per key. There are no quorums or transactions. Reads go to the primary only, and the boot gate stops a rejoining node from serving stale data (decision D6). Concurrent writes to one key are resolved by version, not merged.

3. **How do you order writes without synchronised clocks?**
   With a hybrid logical clock. Every packet carries the sender's HLC and every receiver merges it, so a version is always greater than anything its node has seen. Its physical part follows the most advanced board's clock, so versions roughly match real time. Ties are broken by node id.

4. **What if a packet is lost? Could a write happen twice?**
   Lost packets are retransmitted with the same request id. The receiver's dedup table returns the cached answer for a duplicate instead of executing it again. Only if a whole attempt times out (3 s) does the client start a new attempt with a new id. That can apply a SET twice, but SET and DELETE are idempotent.

5. **What happens to a write when the replica is down?**
   The primary has already stored it, so it answers `REPLICATION_FAILED` (shown as `UNDER_REPLICATED`, still a committed write). Anti-entropy copies it to the replica when one is reachable. Refusing the write would make the store useless exactly when a sensor most needs to record (decision D5).

6. **How does a node that was off catch up, and why can't it serve stale data?**
   It recovers its log, then stays silent (BOOTING) so nobody routes to it, then broadcasts its version table (CATCHUP) while peers push everything newer, and only then becomes ACTIVE. Until ACTIVE it sends no heartbeats and answers `NOT_READY`.

7. **Why tombstones? Why not just delete?**
   If the key were erased, a replica that missed the delete would still hold the old value, and anti-entropy would copy it back. A tombstone has a newer version than the value, so LWW makes the delete win everywhere. Tombstones are dropped only after 10 minutes and confirmation from all four nodes.

8. **What if two nodes write the same key at the same time, or the network splits?**
   Normally only the key's primary writes it, so concurrent writes are serialised there. In a partition, each side may consider a different node the primary and accept writes. When the partition heals, anti-entropy exchanges the versions and every copy keeps the one with the higher (HLC, node id), so all nodes converge on one value. The losing write is discarded: last-writer-wins, not a merge (test `SPLIT` in `cluster_test`).

9. **How do you know it works without the hardware?**
   The same code is compiled for the PC. A simulator runs four nodes with fake EEPROMs and a fake radio that can lose, duplicate and cut links. `make check` runs 31 C++ test programs, including a whole-cluster scenario for every hardware test (H2–H13), plus 34 Python tests (Pi client and the measurement script). The hardware plan repeats them on the boards (`docs/HARDWARE_TEST_PLAN.md`).

10. **What are the limits, and what would you change for a bigger system?**
    * Four nodes with a fixed MAC table, one radio hop, and 64 keys per node (32 KB EEPROM).
    * Node A is a single point of failure for the Pi link (`docs/GATEWAY.md` describes a gateway that removes it).
    * For scale: dynamic membership (gossip), quorum reads/writes for tunable consistency, Merkle trees instead of full version tables, and multi-hop routing.

### Hardware and engineering

11. **Why an external AT24C256 EEPROM instead of the ESP32's own flash?**
    * The EEPROM writes 64-byte pages with no erase step and is rated for 1,000,000 cycles per page. Flash must erase 4 KB sectors and is typically rated for about 100,000 cycles.
    * Small, frequent records therefore fit the EEPROM, and crash reasoning is per page: a record is torn or not, never half a sector.
    * It is separate from the firmware flash, so reflashing a board does not wipe its data.
    * Costs: about 5 ms per page write (`eeprom.h:13-15`, `eeprom.cpp:28-47`) and only 32 KB, hence 64 keys per node.
    * Measured wear: 8.5 years at the 30 s sensor period (`docs/RESULTS.md` §5).

12. **Why ESP-NOW and not Wi-Fi?**
    * ESP-NOW needs no access point, DHCP or connections. The boards talk directly, so the demo works anywhere, even without a campus network.
    * A unicast costs about 1 ms with a MAC-level ACK, and broadcast is free for heartbeats and version tables.
    * TCP over Wi-Fi would add its own retransmissions and hide losses from our reliability layer. Here the layer is ours and testable.
    * Limits we designed around: 250-byte frames (`protocol.h:15`, so the protocol and the 192-byte value limit fit one frame), a single hop, and a fixed channel (`transport.cpp:22`).

13. **Why replication factor 2 and not 3?**
    * With 4 nodes of 64 key slots each, RF = 2 gives room for about 4 × 64 / 2 = 128 distinct keys. RF = 3 gives only about 85.
    * RF = 2 already survives any single node failure without losing data, and a write costs one replication round trip.
    * RF = 3 only pays off with quorum reads and writes (W = 2, R = 2), which the frozen protocol has no fields for.
    * The price: if both owners of a key are down at once, that key is unavailable until one returns. Its data is still in their EEPROMs.

14. **What breaks if you add a fifth node?**
    * The algorithms don't: placement, membership, anti-entropy and GC loop over `node::MIN..node::MAX` (`node_id.h`, `membership.h:10-11`, `hash_ring.h:16-19`, `anti_entropy.cpp:407`), and the alive mask is 8 bits.
    * What is static: `node::MAX = 4`, the MAC table (`transport.cpp:24-38`), the env per node in `platformio.ini`, the console's 4-letter view, and the sensor and Pi node maps.
    * All five boards must be reflashed, because every node must compute the same ring. A mix of 4-node and 5-node images would disagree on placement.
    * After the change about 1/5 of the keys belong to the new node, and anti-entropy hands them over automatically.
    * Joining at run time would need a membership protocol (gossip) and a versioned ring.

15. **What happens on a power loss in the middle of a write, at the hardware level?**
    * A page write inside the AT24C256 takes up to 5 ms. If power drops then, that page's content is undefined.
    * The design never relies on a page write being atomic. Each record (header, key, value, CRC-16) is written without its commit byte, read back and verified, and only then is the commit byte `0xA5` written (`storage.cpp:959-996`).
    * At boot, recovery rebuilds the index from committed records with a valid CRC, and a torn last record is wiped (`storage.cpp:598`).
    * Compaction writes into the other bank and switches by writing one of two superblock copies. The old bank stays authoritative until that switch.
    * `test/storage_manager_test.cpp` cuts power after every single byte of an append and of a full compaction and checks every key. On the boards this is H12 (`HARDWARE_TEST_PLAN.md`).

16. **Node A bridges to the Pi. Isn't that a single point of failure?**
    * For the Pi, yes. If A is off, the Pi loses access, although the cluster keeps working and keeps all data.
    * The bridge only turns Pi frames into ordinary client operations of A (`bridge.cpp:204`), so nothing else depends on A.
    * We gave A the sensor whose driver never disables interrupts (the BH1750) so the Pi link is never held up.
    * `docs/GATEWAY.md` describes the fix: a fifth board as a pure relay that forwards to any live node.

17. **How do you make sure each board runs as the right node?**
    * Node identity is the MAC address. Frames from unknown MACs are dropped (`transport.cpp:127-131`), and a node's id comes from the env it was flashed with.
    * At boot each board prints its MAC, the env, and whether the MAC matches its table entry. On a mismatch it prints `MAC MISMATCH` and which env to flash instead (`main.cpp:152-272`).
    * `scripts/read_macs.sh` produces the table from the boards.

18. **Why a fixed channel 6 and only 8.5 dBm transmit power?**
    * ESP-NOW peers must share a channel, and with no access point there is nothing to follow, so the channel is fixed (`transport.cpp:22`).
    * 8.5 dBm (`transport.cpp:224`) is the usual fix for ESP32-C3 SuperMini boards: their antenna layout misbehaves at full power.
    * The boards sit within 1–2 m, so range isn't needed, and lower power also means less current and fewer brown-out resets.

19. **How do the sensors avoid disturbing the radio and the storage?**
    * One node task owns the radio, storage and sampling, so nothing runs concurrently with a storage write.
    * The DHT11 is read in its own low-priority task, every 2 s with ~5 ms interrupts off, on node D, not on the bridge.
    * The sound sensor (AO) is sampled by its own task: one ADC conversion per 1 ms tick, no interrupts off, no bus lock (the ADC is not on I2C); `status` shows its cost per sample.
    * The BH1750 and the EEPROM share the I2C bus under a lock that an EEPROM write holds until its write cycle has finished (`src/platform/i2c_bus.h`).
    * `BRINGUP.md` §4.8 measures the DHT11's effect: `bench 50` with its bit off vs on.
    * A failing sensor never stalls anything. Its readings become `simulated` (fallback), or `STALE`/`ERROR` for the DHT11.

20. **What did you measure, and how do the simulator and the hardware compare?**
    * `scripts/measure.py` runs the same procedure against both, and the results are in `docs/RESULTS.md`.
    * Simulator numbers:
      * SET p95 about 14 ms;
      * failure detected after 1.5–2.0 s (heartbeat timeout);
      * a client reaches the new owner after about 3.4 s (one attempt deadline);
      * a returning node is caught up and serving after a constant 5.5 s;
      * 0 failed writes at 20 % frame loss (avg ~0.8 s);
      * the hottest EEPROM page lasts about 8.5 years.
    * The measurements also exposed a capacity limit: the 64-entry dedup table caps a node at about 20 writes per second in bursts.
    * Hardware numbers sit beside these, with the reasons they differ: real EEPROM write time, ESP-NOW air time, USB detection delays, the 1.5 s boot delay.

21. **What's the throughput limit, and why?**
    * About **20 writes per second per node** in sustained bursts, set by the dedup window, not by the radio or the EEPROM.
    * Every SET/DEL, and every replication a node receives, takes an entry in its dedup table so a retransmission is answered from the cache instead of running again. The table holds **64 ids** (`MAX_DEDUP_ENTRIES`, `config.h:93`).
    * A completed entry may be reused only after **3.25 s** (`DEDUP_EVICT_AFTER_MS` = attempt deadline 3 s + 250 ms, `config.h:82`), so 64 / 3.25 s ≈ 20 mutations per second.
    * Beyond that the node answers `STORAGE_ERROR` ("dedup table full", `node_runtime.cpp:537`). The client backs off (400 ms × attempt) and retries.
    * Measured: sequential console SETs hit it in **2 of 400 operations**. Those succeeded on the 4th attempt after about 2.4 s; the other 398 needed one attempt (`docs/RESULTS.md` §1 and §6).
    * Why not just shorten the window: an entry must outlive the sender's last retransmission, or a late duplicate would execute twice. The `static_assert` at `config.h:223-228` enforces this.
    * The real knob is the table size: each entry holds a cached reply of up to 64 bytes (`dedup_manager.h:16`), about 85 B in all, so 64 more entries cost about 5 KB of RAM.
    * The other limits are far higher:
      * an EEPROM page write takes ~5 ms (a few hundred writes/s);
      * an ESP-NOW frame takes about 1 ms;
      * each node runs at most 8 client operations at once (`config.h:94`).

22. **What happens if you plug in a disk from another cluster?**
    * It happened: board B's EEPROM still held records from earlier tests. B recovered them as valid data (`storage recovered`), and anti-entropy copied them to their owners as the newest versions (BRINGUP_LOG problem 7).
    * The store cannot tell: records carry a magic number, format version, CRC and HLC version, but nothing says which cluster wrote them (`storage_record.h`, `storage_metadata.h`).
    * Real data is safe from *older* foreign records, because last-writer-wins keeps the newer version. The damage is foreign keys that did not exist here, and foreign records that happen to carry newer HLCs, which would overwrite real values.
    * Fix in practice: `format yes` on every module at first install. We deleted the 20 live foreign keys. GC marked their tombstones once all four nodes had confirmed them, but compacting one node at a time let the others push the tombstones back (BRINGUP_LOG problem 11). They only disappeared when all four compacted at once: 0 foreign keys and 0 tombstones afterwards.
    * Proper fix (DECISIONS D11, not implemented): a deployment id in the superblock and in heartbeats. Storage with another id is formatted at boot, and frames from another deployment are dropped. Not done because it changes the on-disk format on the day of the demo.

23. **What happens when a node's storage is full?**
    * Each node indexes at most 64 keys (`storage.h:16`). A new key with no free slot gets `INDEX_FULL` (`storage.cpp:815`). Updates to keys already stored and all reads keep working.
    * The engine reports it as `STORAGE_ERROR` (`kv_engine.cpp:327`), and `stats` counts it under `storage errors` (`node_runtime.cpp:1036`). A client write whose primary is full fails with `STORAGE_ERROR`; a full replica is a replica failure (D5); anti-entropy repairs to a full owner fail and are retried every round.
    * It happened on the boards: test keys from measure.py and earlier sessions (~113 distinct keys) filled D to 64/64 and C to 62/64 (A and B were full on Tue). On Wed 21 `stats` showed `storage errors` 77 / 24 / 0 / 159 and the logs repeated `anti-entropy: lat/B/3 from A failed (STORAGE_ERROR)`. No client op failed, nothing crashed, nothing was lost (BRINGUP_LOG problem 13, `wed21-health.log`).
    * Capacity: 4 × 64 / RF 2 ≈ 128 distinct keys, less in practice because placement is not perfectly even. Tombstones hold a slot until GC (≥ 10 min, D8).
    * What we did: a fresh format of all four at the same moment, and measure.py now deletes the keys it writes. The confusing part was the name: a capacity limit looked like a hardware error. A distinct `INDEX_FULL` status is future work (DECISIONS D12).

