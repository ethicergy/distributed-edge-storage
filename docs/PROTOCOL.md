# Node protocol

What every node (A–D) runs: all four run the same image and differ only in
their node id. Written from the code (`include/protocol.h`,
`src/node_runtime.cpp`, `src/kv_engine.cpp`, `include/config.h`) and
consistent with [DESIGN.md](DESIGN.md), which explains the reasons behind
it. Firmware: `v1.0-presentation` (`2417e28`).

## 1. Identity and transport

* Node id 1–4 (A–D), fixed at compile time. Peers are a **static MAC
  table** (no discovery); a frame from an unknown MAC is counted and dropped.
* ESP-NOW, channel 6, single hop, frames of at most 250 bytes. Unicast
  frames get a MAC-layer ACK and retry; broadcasts (heartbeats, version
  tables) get neither.
* One node task owns all protocol state; the radio callback only copies
  frames into a 16-deep queue. The task runs one iteration per 1 ms tick.

## 2. Packet format (frozen wire protocol)

Header, 22 bytes, big-endian:

| bytes | field |
|---|---|
| 0 | protocol version (`0x01`) |
| 1 | message type |
| 2 | flags: `REPLICATED`, `RETRY`, `TOMBSTONE` |
| 3 | header length |
| 4–7 | request id |
| 8 | origin node |
| 9 | sender node |
| 10 | destination node (or broadcast) |
| 11 | ttl |
| 12–19 | **HLC**: 48-bit milliseconds + 16-bit counter |
| 20–21 | payload length (≤ 228) |

| type | payload |
|---|---|
| `SET_REQ`, `REPL_REQ`, `RESYNC_DATA` | key (≤ 32 B) + value (≤ 192 B) |
| `GET_REQ`, `DEL_REQ` | key |
| `SET_ACK`, `DEL_ACK`, `REPL_ACK` | status + key |
| `GET_RESP` | status + key + value |
| `HEARTBEAT` | empty |
| `VERSION_TABLE` | sync id, chunk index / count, up to 5 entries of {tombstone flag, key, version HLC, version origin} |

**Request identity** = (origin node, request id). The sender is not part of
it, so a relayed or retransmitted request keeps its identity.

Status codes: OK, NOT_FOUND, INVALID_REQUEST, KEY_TOO_LARGE,
VALUE_TOO_LARGE, STORAGE_ERROR, NOT_RESPONSIBLE, REPLICATION_FAILED,
STALE_VERSION, UNSUPPORTED.

## 3. Clock rules (every send and receive)

* Every outgoing packet carries the node's current HLC.
* On **every** received packet, heartbeats included:
  * **adopt physical:** if the packet's physical milliseconds are ahead,
    move the node's physical offset forward to match (never backwards);
  * **merge:** HLC receive rule (maximum of both, counter adjusted).
* **Version of a write** = (HLC, origin). Versions compare
  lexicographically, and the **higher one wins everywhere**
  (last-writer-wins).
* At boot, the clock starts at or above the highest version persisted in
  the EEPROM.

## 4. Lifecycle (boot gate)

1. **BOOTING.** Recover storage. Send nothing (no heartbeats, so nobody
   routes to this node). Listen for 2.5 s (membership timeout + one
   heartbeat), merging every clock heard. If no peer was heard, go straight
   to ACTIVE (cold cluster or lone node).
2. **CATCHUP (3 s).** Broadcast the full version table twice. Peers push
   everything this node owns and lacks or holds older. Client requests are
   answered `NOT_RESPONSIBLE`, uncached, so clients go to the other owner.
   Replication and resync data **are** applied (more data is always safe
   under LWW).
3. **ACTIVE.** Send heartbeats and serve as primary and replica.

## 5. The main loop (every iteration, in this order)

1. **State:** advance the boot gate.
2. **View:** a peer is alive if heard within **2000 ms**; recompute the view,
   logging changes (`view: AB-D`).
3. **Heartbeat:** if ACTIVE, broadcast `HEARTBEAT` every **500 ms**.
4. **Client operations:** start due attempts, retransmit, time out, back off.
5. **Replications:** retransmit `REPL_REQ`, enforce the 1.5 s deadline.
6. **Anti-entropy:** every **10 s**, broadcast the version table; drain the
   push queue, paced 40 ms apart.
7. **Storage:** advance an incremental compaction by one step, or start one
   (less than 3 KB free, or garbage-collection pressure).
8. **Completions:** deliver finished operations to the console or Pi bridge.

## 6. Receive path

1. **Drop the frame if:** it does not decode; the link-layer source MAC is
   not the header's sender (spoofed); it is the node's own frame; or the
   destination is neither this node nor broadcast.
2. **Merge the HLC** (§3).
3. **Dispatch by type:**
   * `HEARTBEAT` → mark the sender alive
   * `SET_REQ` / `GET_REQ` / `DEL_REQ` → serve as primary (§8)
   * `SET_ACK` / `GET_RESP` / `DEL_ACK` → complete a client attempt (§7)
   * `REPL_REQ` → serve as replica (§9)
   * `REPL_ACK` → complete a replication (§8)
   * `VERSION_TABLE` / `RESYNC_DATA` → anti-entropy (§10)

## 7. As a client (any node; the console, the sampler and the Pi bridge use this)

An operation (`SET`, `GET`, `DEL`) gets **up to 5 attempts**. Each attempt:

1. **Placement**, recomputed every attempt from the current view: FNV-1a hash
   of the key onto a ring of 4 × 64 virtual points. **Primary** = first live
   node clockwise; **replica** = next live node on a different board.
2. **Target** = the primary, or the replica if the previous attempt got
   `NOT_RESPONSIBLE` from that primary.
3. **New request identity** (origin = this node, next request id).
4. If the target is this node, the same server code runs locally, with no
   radio.
5. Otherwise unicast the request and retransmit with the **same identity**
   (flag `RETRY`) after 600 ms, then 1200 ms: at most 3 retransmissions, with
   a **3 s attempt deadline**.

| result | action |
|---|---|
| OK, NOT_FOUND | done |
| REPLICATION_FAILED | done; reported as `UNDER_REPLICATED` (the write is committed) |
| NOT_RESPONSIBLE | next attempt goes to the other owner, after backoff |
| STALE_VERSION | new attempt immediately |
| STORAGE_ERROR, or the attempt deadline | new attempt after backoff |

Backoff before attempt *n* is *n* × 400 ms. The total, about 4 s, outlasts a
membership timeout, so the last attempts always see a fresh view.

**Exactly once** holds per request identity: retransmissions are
deduplicated. A new attempt is a new identity, but SET and DEL have the same
effect if applied twice.

## 8. As a primary (on `SET_REQ`, `DEL_REQ`, `GET_REQ`)

1. **Not ACTIVE** → `NOT_RESPONSIBLE`, not cached.
2. **Dedup** (SET and DEL only; GETs are idempotent):

   | dedup result | action |
   |---|---|
   | duplicate, still in progress | ignore |
   | duplicate, completed | replay the cached answer |
   | table full (64 entries) | answer `STORAGE_ERROR`, never silence |
   | new | continue |

3. **Ownership:** placement in *this node's* view must name it as primary,
   else `NOT_RESPONSIBLE`. A key of length 0 → `INVALID_REQUEST`; over 32 B
   → `KEY_TOO_LARGE`.
4. **GET:** read the newest record from the EEPROM and answer `GET_RESP`
   (OK or NOT_FOUND). Done.
5. **SET / DEL:**
   1. Assign the version = (`on_local_event()` HLC, this node). The clock has
      already merged the request's HLC.
   2. Append to the EEPROM log with write-verify; a DEL appends a
      **tombstone**.
   3. Send `REPL_REQ` to the replica in the current view, with the **version
      in the header** (HLC, origin = primary) and a new replication identity.
      Retransmit after 300 ms, then 600 ms, with a **1.5 s deadline**.
6. **Answer the client by the replica's response**, and cache the answer in
   the dedup table:

   | replica response | client gets |
   |---|---|
   | OK | OK |
   | STALE_VERSION | STALE_VERSION: the replica holds a newer version; the client's retry wins, because the clock has now merged it |
   | error, timeout, or no live replica | REPLICATION_FAILED: committed on the primary only; anti-entropy copies it later |

There is no rollback: the log is append-only and LWW.

## 9. As a replica (on `REPL_REQ`, in any state)

1. **Dedup** by the replication identity (same rules as §8; a cached
   `REPL_ACK` is replayed).
2. **Apply LWW:** if the incoming version (HLC, origin) is newer than the
   stored one, append it (value or tombstone) and answer OK; otherwise answer
   `STALE_VERSION`.
3. `STORAGE_ERROR` if the EEPROM fails or the log is full during compaction.
4. Cache the `REPL_ACK` and send it.

## 10. Anti-entropy, hand-off and garbage collection

* **Announce:** every 10 s (and twice in CATCHUP), broadcast the version table
  as chunks of 5 entries, all with one sync id.
* **On each received entry** from sender S:
  * this node's version is newer and S owns the key (counting S as alive)
    → queue a `RESYNC_DATA` push to S;
  * this node's version is older or equal → nothing; S pushes when it
    processes this node's table.
* **After all chunks of one sync id:** push every key this node holds that S
  did not list and S owns.
* `RESYNC_DATA` is applied with LWW and **no ownership check**, and is not
  acknowledged; a lost push is redone next round.

This one mechanism covers an under-replicated write reaching its replica, a
rebooted node catching up, **hand-off** when the view changes (a new owner
lacks keys, so it gets pushes), and a wiped node being rebuilt.

**Garbage collection** uses only complete tables received in the last 30 s
and compared after this node's last local change to the key:

* **orphan:** a key this node no longer owns, where every current owner
  reported a version ≥ its own → drop the copy;
* **tombstone:** older than 10 minutes, all four nodes alive, and every other
  node reported no copy or a version ≥ its own → drop it.

Drops are applied by the next compaction, and tombstones marked for GC are
not pushed again. The 10-minute age means no late replication can resurrect
a deleted key.

## 11. Local storage contract (what "append" means)

* Two 16 KB banks plus a double superblock. Each record is header + key +
  value + CRC-16, plus a commit marker written **last**, after a verified
  write of the body. A RAM index of up to 64 keys is rebuilt at boot by
  replaying the active bank.
* After a power cut, a torn tail is wiped and appends resume; anything else is
  treated as corruption: auto-format, and anti-entropy refills the node.
* Compaction is incremental (one step per loop): copy the current record of
  each key into the other bank, then switch banks atomically through the
  superblock. The old bank stays authoritative until that switch.

## 12. Invariants the timeouts guarantee (`static_assert`s in `config.h`)

* Replication deadline 1.5 s + primary budget 0.5 s < client attempt deadline
  3 s: a primary whose replica died answers `REPLICATION_FAILED` before the
  client gives up the attempt.
* Client attempt deadline 3 s < dedup eviction 3.25 s < dedup retention 6 s:
  a retransmission always finds its dedup entry, so it is never executed
  twice.
* Backoff total 4 s > membership timeout + heartbeat 2.5 s: a client's last
  attempts use a view that has noticed a dead node.

**Consistency model:** eventually consistent, last-writer-wins per key,
replication factor 2. A committed write is durable on at least one node and
reaches both owners. Reads go to the primary. Not linearizable: no quorum,
no transactions.
