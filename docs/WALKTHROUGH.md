# Code walkthrough (for the demo and viva)

A guided tour of edge-kv through four questions: how a write travels, how a sensor reading reaches the Pi, what a power failure does, and which distributed-systems ideas are used where. The examiner questions are at the end.

File:line references are to branch `hw-bringup`. Three facts make the rest easy to follow:

* **Every node runs the same code.** Only `NODE_ID` differs (`platformio.ini`). All behaviour comes from what each node computes from its own view of who is alive.
* **One task does everything.** The node task in `src/platform/main.cpp:347-388` loops forever and calls `runtime.on_frame` for received radio frames, the console, the Pi bridge (on A), `runtime.tick`, `sampler.tick` and `console.tick`. The ESP-NOW receive callback only copies frames into a queue (`src/platform/transport.cpp:114-157`). So the distributed-store state needs no locks.
* **Everything outside `src/platform/` is plain C++17.** The same code runs four times inside one PC process in the simulator (`sim/cluster.cpp`). That is how every scenario below is tested (`test/cluster_test.cpp`).

---

## 1. The life of `set k v` typed on node B

The ring puts key `k` on **primary D, replica B** (`ring k` on any console). So this one command touches B → D → B → D → B.

### On B: the console and the client side

| # | what happens | where |
|---|---|---|
| 1 | Characters arrive over USB. Enter dispatches the line. | `main.cpp:361-363` → `Console::on_char` (`src/console.cpp:132`) → `Console::execute` (`console.cpp:157`, `"set"` at `:179`) |
| 2 | `cmd_kv` splits key and value and calls the runtime's client API. | `console.cpp:371-401` → `Runtime::submit` (`src/node_runtime.cpp:1205`) |
| 3 | `submit` checks the node is ACTIVE and the sizes (key ≤ 32 B, value ≤ 192 B). It copies the operation into a free **client-op slot** (8 of them), phase WAITING. Nothing is sent yet. | `node_runtime.cpp:1217-1265` |
| 4 | On the next `runtime.tick` → `tick_client_ops` → `start_attempt`. | `node_runtime.cpp:1656`, `:1588`, `:1294` |
| 5 | **Placement:** `placement_.locate(key, view)` hashes the key with FNV-1a and walks the ring of 4 × 64 virtual points clockwise from that hash. The first live node is the primary (D), the next different live node is the replica (B). `view` is B's own set of live nodes. | `node_runtime.cpp:1312`; `src/placement.cpp:55-150`; `src/consistent_hash.cpp` (FNV-1a); `src/hash_ring.cpp:58` (ring built once at boot) |
| 6 | A new **request identity** `(origin = B, request_id)` and an **HLC timestamp** for the packet. | `ids_.next()` (`src/request_identity.cpp:59`); `clock_.on_local_event()` (`src/hlc.cpp:106`); packet built by `packet::build_request` (`src/packet_builder.cpp:49`) into the frozen 22-byte header format (`include/protocol.h`) |
| 7 | Target D ≠ B, so the op becomes REMOTE. The **request manager** takes the packet and sends it, then keeps a copy for retransmission. | `client_requests_.submit` (`src/request_manager.cpp:93`) → `Runtime::reliable_send` (`node_runtime.cpp:1129`) → `send` (`:1112`) → `EspNowLink::unicast` (`main.cpp:71`) → `EspNowTransport::send_unicast` (`transport.cpp:307`) → `esp_now_send` to D's MAC |
| 8 | If no answer arrives in 600 ms, it retransmits the same packet with `FLAG_RETRY`, backing off up to 1200 ms, at most 3 times. The attempt deadline is 3 s. | `request_manager.cpp:201-283`; timings `include/config.h:47-59` |

### On D: the primary

| # | what happens | where |
|---|---|---|
| 9 | The ESP-NOW callback maps the sender MAC to node B and queues the frame. The node task hands it to the runtime. | `transport.cpp:114-157`; `main.cpp:355-357` |
| 10 | `on_frame` validates the header, rejects a frame whose header sender differs from the radio sender (spoof check), and **merges B's HLC** into D's clock. | `node_runtime.cpp:335-384` |
| 11 | SET_REQ → `handle_client_request`. If D isn't ACTIVE it answers `NOT_READY` (boot gate). | dispatch `node_runtime.cpp:393-396`; gate `:453-466` |
| 12 | **Dedup:** `begin_request((B, id))`. A new request is marked IN_PROGRESS. If it's a duplicate, the cached reply is replayed instead of running the request again (this is the exactly-once part). | `node_runtime.cpp:472-534`; `src/dedup_manager.cpp:137` |
| 13 | **Version:** `version = (clock_.on_local_event(), D)`. D's clock has already merged B's, so the version is newer than anything either has seen. | `node_runtime.cpp:545-551` |
| 14 | `engine_.execute` checks that D really is the primary in **D's own view**. If not, it answers `NOT_RESPONSIBLE`. Then `execute_set` → `storage_.set` → `apply`. | `src/kv_engine.cpp:71-144`, `:231`; `src/storage.cpp:1528`, `:1411` |
| 15 | **Last-writer-wins check:** `compare_versions(new, stored)` compares the HLC first, then the origin id. Not newer → `STALE_VERSION`. | `storage.cpp:1462-1475`, `compare_versions` at `storage.cpp:85` |
| 16 | **Append to EEPROM:** a record (header, key, value, CRC-16) is written **without its commit byte**, read back and verified, then the commit byte `0xA5` is written last. A power cut before that byte leaves a record that recovery ignores. The RAM index (64 slots) is updated. | `append_record_to_bank` (`storage.cpp:866`; body `:959`, verify `:971`, commit `:996`); layout `include/storage_record.h`; driver `src/platform/eeprom.cpp:163` (64-byte pages, ACK polling) |
| 17 | The local change is noted for anti-entropy. Because the response names replica B, D starts **replication**: a REPL_REQ carrying the version (`hlc` + origin D) and `FLAG_REPLICATED`, through its own request manager (300 ms retries, 1.5 s deadline). | `node_runtime.cpp:574-586` → `begin_replication` (`:657-747`) |

### On B again: the replica

| # | what happens | where |
|---|---|---|
| 18 | REPL_REQ → `handle_replication_request`. It goes through dedup as well, so a retransmitted REPL_REQ is applied once. | `node_runtime.cpp:883-930` |
| 19 | `engine_.apply_versioned` stores the value **with D's version**, using the same LWW `storage::apply` as on D. | `node_runtime.cpp:937`; `kv_engine.cpp:146-229` |
| 20 | It builds a REPL_ACK, caches it in dedup, and sends it to D. | `node_runtime.cpp:965-999` |

### On D: completing the write

| # | what happens | where |
|---|---|---|
| 21 | REPL_ACK → `handle_replication_ack` → `finish_replication(OK)`. | `node_runtime.cpp:749-819`, `:821` |
| 22 | `respond_to_client` builds the SET_ACK, **caches it in dedup** under `(B, id)` so a retry gets the same answer, and delivers it to the origin, B. | `node_runtime.cpp:601-650`; builder `src/kv_response.cpp:159` |
| 23 | If B (as replica) had not answered within 1.5 s, D would answer `REPLICATION_FAILED`. The write stays committed on D, and anti-entropy copies it later (decision D5). | `node_runtime.cpp:838-881` |

### On B: the reply

| # | what happens | where |
|---|---|---|
| 24 | SET_ACK → `handle_client_response`: it matches `(B, id)` to the client-op slot and stops the retransmissions. | `node_runtime.cpp:1478-1531` |
| 25 | `on_op_response` → `finish_op` fills a `ClientResult`: status, served by D, attempts, latency. | `node_runtime.cpp:1424`, `:1534` |
| 26 | At the end of the tick, `deliver_completions` frees the slot and calls the console's completion callback, which prints the line. | `node_runtime.cpp:1631-1653` → `Console::on_complete`/`handle_result` (`console.cpp:513-562`) |

```
SET k: OK (served by D, 1 attempt(s), 11 ms)
```

**Variations worth knowing:**
* If B itself were the primary, step 7 takes the **local fast path**: the same server code runs without the radio (`node_runtime.cpp:1351-1365`).
* `get` is the same trip without replication. GET is not deduplicated because it is idempotent (`node_runtime.cpp:467-475`).
* `del` writes a **tombstone** instead of a value.

---

## 2. A sensor reading on its way to the Pi

Example: node D's temperature (DHT11). The key `s/D/temp` lives on **primary B, replica D**.

### Measuring and storing (on D)

| # | what happens | where |
|---|---|---|
| 1 | A separate low-priority task reads the DHT11 every 2 s and records ok/failed and the last good value under a spinlock. Only when D's bit in `REAL_SENSOR_NODES` is set; otherwise `SimulatedSource` provides values. | `src/platform/sensors.cpp:79-102` (task), `:64` (`take`); switch `include/config.h:151`; `main.cpp:98-115` |
| 2 | Every node-task iteration calls `sampler.tick`. Every 30 s (`SENSOR_PERIOD_MS`), staggered per node, it reads the source. | `main.cpp:383` → `Sampler::tick` (`src/sensor.cpp:398`) |
| 3 | `FallbackSource::read` → `LatestValueSource::read` takes the DHT task's results. It keeps the last good value, and after 10 s without a good read flags it `STALE` (or `ERROR` if there was never one). It is never replaced by simulated data. | `src/sensor_sources.cpp:225`, `:160` |
| 4 | The sample is encoded as a 20-byte reading: kind, flags, sequence, value in thousandths, HLC of the sample. Event kinds (motion, sound) use 24 bytes. | `sensor.cpp:87` (`encode`); format comment `include/sensor.h:51-81` |
| 5 | Two ordinary client SETs: `s/D/temp` (latest) and `h/D/temp/<seq % 4>` (4-slot history ring). From here it is **exactly the trip in part 1**: primary B stores and replicates to D. | `sensor.cpp:485-522` → `Runtime::submit` |
| 6 | After a reboot, the sampler first reads its own latest key back from the cluster, so sequence numbers continue. | `sensor.cpp:411-436`, `Sampler::handle` `:365` |

### Reading it on the Pi (`python3 -m edgekv sensors`)

| # | what happens | where |
|---|---|---|
| 7 | The CLI asks the client for each node's latest key, e.g. `GET s/D/temp`. | `pi/edgekv/cli.py` `cmd_sensors` → `client.py:168` → `sensor` `:164` → `get` `:148` |
| 8 | `_transact` builds a GET_REQ packet with a Pi-chosen request id, frames it `AA 55 \| len \| packet \| CRC-8`, and writes it to the UART. | `client.py:90-130`; `pi/edgekv/framing.py` |
| 9 | On node A, the node task reads UART1 (GPIO20) and feeds the bridge. The frame decoder resynchronises on noise such as boot text. | `main.cpp:365-379` → `Bridge::on_bytes` (`src/bridge.cpp:50`); decoder `src/uart_frame.cpp` |
| 10 | `handle_envelope` turns the Pi's packet into an ordinary **client op of node A** (`runtime_.submit`) and remembers the Pi's request id. | `bridge.cpp:126-223` (submit at `:204`) |
| 11 | A's runtime sends GET_REQ to B (primary of `s/D/temp`). B answers GET_RESP with the 20 bytes. This is part 1's path without replication. | as in part 1 |
| 12 | The completion goes to `Bridge::on_complete`, which answers the Pi with **the Pi's request id** and A's current HLC in the header. | `bridge.cpp:225`, `respond` `:88`, `send_envelope` `:77` → `bridge_uart.write` (`main.cpp:139`) |
| 13 | On the Pi: the frame is decoded and matched to the request id. The bridge's HLC calibrates `offset = pi_wall_ms − hlc_ms`. | `client.py:118-121`, `_calibrate` `:85` |
| 14 | `reading.decode` unpacks the 20 bytes. `wall_time(hlc)` turns the sample's HLC into Pi wall-clock time. The CLI prints it, and `watch --csv` appends a CSV row. | `pi/edgekv/reading.py:92`; `client.py:180`; `cli.py:43` (`_format_reading`), `:53` (`_csv_row`) |

---

## 3. Node C loses power and comes back

### While C is off

| t | what happens | where |
|---|---|---|
| 0 | C stops sending heartbeats. Every node normally broadcasts one every 500 ms while ACTIVE. | `node_runtime.cpp:1748-1772`; `config.h:22` |
| 0–2 s | Requests and replications sent to C go unanswered and are retransmitted. | `request_manager.cpp:201` |
| ~2 s | On every other node `membership.is_alive(C)` turns false (2 s without a heartbeat). `tick_view` logs `view: AB-D`. | `src/membership.cpp:85-121`; `node_runtime.cpp:1724-1746`; `config.h:25` |
| after | **Placement changes on its own:** a key whose primary was C now maps to the next live node on the ring. Only C's keys move. A client attempt that timed out is retried (up to 5 attempts, 400 ms × n backoff) with placement recomputed, so it reaches the new owner. | `node_runtime.cpp:1387-1420` (`attempt_failed`), `:1312` (`locate` per attempt) |
| after | Writes whose replica is C: the primary times out after 1.5 s and answers `REPLICATION_FAILED`, which the client sees as `UNDER_REPLICATED`. The write is committed and will be repaired. | `node_runtime.cpp:838-881`; decision D5 |
| after | Anti-entropy rounds (every 10 s) also **hand off** existing copies to the new owners. | `node_runtime.cpp:1824`; `src/anti_entropy.cpp:227-347` |

### When C powers back on

| step | what happens | where |
|---|---|---|
| 1 | **Storage recovery:** read both superblock copies and pick the valid, newer bank. Scan its records and rebuild the RAM index from committed records. A torn last record (power cut mid-write) is discarded and wiped. | `storage.cpp:1702` (`begin`), `:598` (`recover_active_bank`) |
| 2 | **Clock rule:** the HLC starts at or above the highest version stored, so C never issues an older version than one it already holds. | `node_runtime.cpp:271-276`; `hlc.cpp:23` |
| 3 | **BOOTING (2.5 s):** C listens and sends no heartbeats, so no one routes requests to it. Its clock merges everyone else's. | `tick_state` `node_runtime.cpp:1673-1696`; heartbeats only when ACTIVE `:1750` |
| 4 | **CATCHUP (3 s):** peers were heard, so C broadcasts its **version table** twice (key + version per entry, in chunks). Client requests sent to it get `NOT_READY`. | `node_runtime.cpp:1700-1717`; `anti_entropy.cpp:71` (`start_round`); gate `node_runtime.cpp:453` |
| 5 | Each peer compares C's table with its own. For every key C owns where the peer is newer, or that C lacks entirely (complete table), it queues a **push**: a RESYNC_DATA packet carrying the value and its original version. | `anti_entropy.cpp:227-347`; `node_runtime.cpp:1775-1822` (`send_push`) |
| 6 | C applies each push through the same LWW `storage::apply` and logs `anti-entropy: key k6 repaired from A`. Tombstones are pushed the same way, so a key deleted while C was off stays deleted. | `node_runtime.cpp:1002-1047` (`handle_resync`) → `kv_engine.cpp:146` |
| 7 | **ACTIVE:** C heartbeats again. Within ~1 s every view is `ABCD`, placement maps C's keys back to C, and C serves them, already up to date. Test H7H8 checks that a stale value is never returned during this. | `node_runtime.cpp:307-324`; `test/cluster_test.cpp` `H7H8` |
| 8 | **Cleanup:** the nodes that stood in for C now hold **orphan copies**. Garbage collection drops them once both current owners report a version at least as new. Tombstones are dropped after 10 min, once all four nodes confirm. | `anti_entropy.cpp:389-471`; decision D8 |

If C's EEPROM was wiped or corrupted, the same steps rebuild it from scratch (`STORAGE_AUTO_FORMAT_ON_CORRUPTION`, `node_runtime.cpp:233-243`; test `H11`).

---

## 4. Cheat sheet: concepts and where they live

| concept | in two sentences | code |
|---|---|---|
| **Consistent hashing** | Keys and nodes are placed on the same hash ring (64 virtual points per node); a key belongs to the first live node clockwise, and its replica to the next different node. When a node fails, only its keys move, unlike `hash mod N`, which would move most keys. | `src/consistent_hash.cpp` (FNV-1a), `src/hash_ring.cpp`, `src/placement.cpp:55-150`; `ring <k>` on the console |
| **Replication (RF = 2)** | The primary stores a write, then sends REPL_REQ to the replica and answers the client after the replica's ACK. If the replica is down, the write stays committed on the primary as `UNDER_REPLICATED` and is repaired later. | `node_runtime.cpp:657-881` (primary), `:883-1000` (replica); decision D5 |
| **Hybrid logical clock (HLC)** | A 64-bit timestamp: 48 bits of milliseconds plus a 16-bit counter. Every packet carries it and every receiver merges it. Versions then follow causal order and stay close to real time, even though the boards have no wall clock. | `src/hlc.cpp` (`on_local_event` `:106`, `on_receive` `:154`, `adopt_physical` `:256`); merged in `node_runtime.cpp:381-384` |
| **Last-writer-wins (LWW)** | Every stored value carries a version (HLC, origin node). A write or repair is applied only if its version is strictly newer, with ties broken by node id, so all copies converge to the same value without coordination. | `compare_versions` `storage.cpp:85`, check in `storage::apply` `:1462-1475` |
| **Tombstones** | A delete is stored as a versioned "deleted" record rather than erased, so an older copy elsewhere cannot bring the key back. They are garbage-collected after 10 min once every node has confirmed them. | `State::TOMBSTONE` in `include/storage.h`; `FLAG_TOMBSTONE` in replication/resync; GC `anti_entropy.cpp:389`; test `H9` |
| **Dedup / exactly-once** | Each request has an identity (origin node, request id) and retries reuse it. The server remembers recent identities with their cached reply, so a retransmitted or duplicated request is answered from the cache instead of being executed twice. | `src/dedup_manager.cpp`; used at `node_runtime.cpp:472-534` (client) and `:899-932` (replica); retries in `src/request_manager.cpp`; tests `H4`, `H5` |
| **Anti-entropy** | Every 10 s, and during catch-up, each node broadcasts a table of its keys and versions, and whoever holds a newer copy of a key the sender owns pushes it. This one mechanism repairs under-replicated writes, catches up rebooted nodes, hands keys off after placement changes, and rebuilds a wiped node. | `src/anti_entropy.cpp`; `node_runtime.cpp:1775-1889`, `:1002-1062`; tests `H6`, `H11` |
| Membership / failure detection | Heartbeat every 500 ms; a peer is dead after 2 s of silence. Each node keeps its own view, with no leader and no agreement round. | `src/membership.cpp`, `node_runtime.cpp:1748`; `config.h:22-25` |
| Boot gate | A rebooted node stays silent, then catches up, then serves. This prevents stale reads. | `node_runtime.cpp:1673-1722`; decision D6 |
| Crash-safe storage | Append-only log in two banks with CRC and a commit byte written last; compaction switches banks atomically through a superblock. | `src/storage.cpp`; `test/storage_manager_test.cpp` cuts power at every byte |

---

## 5. Likely examiner questions

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

8. **What happens on a power cut in the middle of an EEPROM write?**
   Each record is written without its commit byte, verified, and committed last. Recovery ignores and wipes an uncommitted tail. Compaction copies into the other bank and switches over atomically through a two-copy superblock. A test cuts power at every byte (`test/storage_manager_test.cpp`).

9. **How do you know it works without the hardware?**
   The same code is compiled for the PC. A simulator runs four nodes with fake EEPROMs and a fake radio that can lose, duplicate and cut links. `make check` runs 31 C++ test programs, including a whole-cluster scenario for every hardware test (H2–H13), plus 27 Python tests of the Pi client. The hardware plan repeats them on the boards (`docs/HARDWARE_TEST_PLAN.md`).

10. **What are the limits, and what would you change for a bigger system?**
    * Four nodes with a fixed MAC table, one radio hop, and 64 keys per node (32 KB EEPROM).
    * Node A is a single point of failure for the Pi link (`docs/GATEWAY.md` describes a gateway that removes it).
    * For scale: dynamic membership (gossip), quorum reads/writes for tunable consistency, Merkle trees instead of full version tables, and multi-hop routing.
