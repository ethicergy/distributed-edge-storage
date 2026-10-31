# edge-kv design

A leaderless, replicated key-value store on four ESP32-C3 boards
(nodes A=1, B=2, C=3, D=4). Each node has an AT24C256 32 KB EEPROM,
talks to the others over ESP-NOW (channel 6, static MAC table, single
hop), places keys with a consistent-hash ring (RF = 2), versions every
mutation with a hybrid logical clock, and repairs divergence with
anti-entropy. A Raspberry Pi reaches the cluster through a UART bridge
on node A.

The wire protocol (`include/protocol.h`) is frozen: 22-byte header,
11 message types, the status table. Nothing below adds fields to it.

Decisions that were open in the completion prompt (D0–D8) are recorded
in [DECISIONS.md](DECISIONS.md). What a node does, step by step and byte
by byte, is in [PROTOCOL.md](PROTOCOL.md).

---

## 1. Module map

```
                       +---------------------------------------------+
  USB console ------>  |  console          (include/console.h)       |
  Pi (UART1) ------->  |  bridge + uart    (bridge.h, uart_frame.h)  |
  sensor source ---->  |  sensor sampler   (sensor.h)                |
                       +----------------------+----------------------+
                                              | submit(SET/GET/DEL)
                       +----------------------v----------------------+
                       |  node runtime     (node_runtime.h)          |
                       |   client ops: routing, attempts, fast path  |
                       |   server: dedup -> kv engine -> replication |
                       |   membership + boot/catch-up gate           |
                       |   anti-entropy tracker + GC (anti_entropy.h)|
                       |   HLC (hlc.h)    request manager x2, dedup  |
                       +-------+----------------------+--------------+
                               |                      |
                     +---------v--------+   +---------v---------+
                     | storage manager  |   | edge::Link        |
                     | (storage.h)      |   |  ESP-NOW (fw)     |
                     | eeprom::Device   |   |  sim network(host)|
                     +---------+--------+   +-------------------+
                               |
                     AT24C256 (fw) / FakeEeprom (host)
```

Everything except `src/platform/` is plain C++17 with no Arduino
dependency. The same runtime runs on the boards and, four times over,
in the host simulator (`sim/`), which is how the system-level tests
(`test/cluster_test.cpp`) exercise failures without hardware.

Rules kept from the architecture:

| Rule | Where it holds |
|------|----------------|
| R1 only storage touches the EEPROM | `StorageManager` is the only user of `eeprom::Device` |
| R2 protocol knows nothing of ESP-NOW/EEPROM | `protocol.*`, `packet_builder.*` are pure; placement sees ids + membership only |
| R3 RX callback only enqueues | `src/platform/transport.cpp` `on_receive` copies into a FreeRTOS queue |
| R4 retries keep identity/HLC/payload, set FLAG_RETRY; one retry system | `request::Manager` + `prepare_retry`, used for client requests **and** REPL_REQ |
| R5 every node computes placement itself | `placement::Resolver` on each node, same ring |
| R6 sensor writes use the normal SET path | `sensor::Sampler` calls `Runtime::submit` |
| R7 header and code tables frozen | unchanged; see "Mapping outcomes to frozen codes" |
| R8 timing/capacity in one header | `include/config.h`, ordering enforced by `static_assert` |

## 2. Task model (firmware)

One FreeRTOS task (`node`, 16 KB static stack) owns the runtime and
also runs the sampler, the console and the bridge. Nothing else touches
runtime state, so there are no locks. The ESP-NOW callbacks run in the
Wi-Fi task and only copy frames into a 16-deep queue; overflows,
unknown source MACs and per-peer MAC-layer TX results are counted
(`stats`). The loop yields one tick (1 ms) per iteration.

The longest blocking step is one EEPROM append with write-verify
(about 10–30 ms). Compaction is incremental (a few pages or one record
per step), so heartbeats are never delayed by more than one step.

## 3. Placement

FNV-1a key hash on a ring of `4 x 64` virtual points (mix32 of FNV-1a
over `(node, index, C3, 28)`). Primary = first live node clockwise,
replica = next live node that is a different board.

64 points per node (was 4) because 4 produced only 6 of the 12 possible
primary/replica pairs, so a failed node's whole load went to one
neighbour. With 64: all 12 pairs occur, primary share is 21–30 % per
node, owner share at most 27 % (`test/ring_balance_test.cpp`).

`placement::Resolver::locate` takes either the membership view or an
explicit alive mask; anti-entropy uses the mask form to ask "who owns
this key if node S counts as alive".

## 4. Membership and the boot gate

Heartbeat broadcast every 500 ms; a peer is alive if heard within
2000 ms. Each node has its own view; views differ briefly during
changes and that is handled by LWW, not by agreement.

A node starts in **BOOTING**: silent (no heartbeats, so nobody routes
to it), listening for 2.5 s and merging every clock it hears. If any
peer is alive it moves to **CATCHUP**: it broadcasts its full version
table twice over 3 s, and peers push whatever it owns and lacks or has
older (see §8). Then **ACTIVE**: heartbeats start and it serves as
primary/replica. A lone node or a cold-started cluster goes straight
to ACTIVE.

While not ACTIVE, client requests addressed to it are answered
`NOT_RESPONSIBLE` (uncached), so the client tries the other owner.
REPL_REQ and RESYNC_DATA are applied in every state (more data is
always safe under LWW).

## 5. Clocks and versions

HLC: 48-bit physical milliseconds + 16-bit logical counter
(`hlc.h`). The physical source is the board's uptime, so on its own it
means nothing across boards. Two additions make it a usable cluster
clock:

* **Merge on every packet**, heartbeats included (`on_receive`).
* **Physical adoption** (`adopt_physical`): when a packet carries a
  physical time ahead of ours, our physical offset moves forward to it
  (never backwards). All clocks therefore track the most advanced
  board's millisecond clock; versions from different boards are ordered
  by real time to within one heartbeat.

There is no "remote drift" rejection: with uptime-based clocks,
rejecting far-ahead clocks would permanently split a cluster whose
halves booted at different times.

Reboot rule: the clock starts at or above the highest persisted version,
and the BOOTING window merges peers' clocks before the node serves.

**Version = (HLC, origin)**, compared lexicographically. The **primary**
assigns it: `hlc = on_local_event()` after merging the request's HLC,
`origin = primary's node id`. One clock never repeats a value, so two
nodes that briefly both act as primary for a key still produce distinct
versions. REPL_REQ and RESYNC_DATA carry the version in the header
(`hlc`, `origin_node_id`).

Wall-clock time (decision D4): the Pi converts a sample's HLC with the
offset between its own clock and the HLC in the bridge's latest answer
(`EdgeKV.wall_time`).

## 6. Write and read path

```
client (any node)                 primary P                  replica R
  submit(SET k v)
  locate(k) -> P
  [P == self: same code, no radio]
  SET_REQ (id=(C,n), hlc) ------->  dedup NEW
                                    v = (hlc, P)
                                    EEPROM append (verify)
                                    REPL_REQ (id=(P,m), hlc=v) ---->  dedup NEW
                                                                      apply LWW, append
                                    <-------------------------------- REPL_ACK OK
  <------------------------------- SET_ACK OK (cached in dedup)
```

Outcomes on the primary (decision D5):

| replica answer | client gets | meaning |
|---|---|---|
| OK | OK | on both owners |
| STALE_VERSION | STALE_VERSION | the replica holds a newer version; this write lost. The primary's clock merged the ACK's HLC (>= that version), so the client's next attempt wins |
| error / no answer within 1.5 s / no live replica | REPLICATION_FAILED | committed on the primary only; anti-entropy copies it later. The client API reports `UNDER_REPLICATED` (a committed write) |

There is no rollback: with LWW and an append-only log there is nothing
to roll back to, and the write is durable on one node.

Reads go to the primary only. GETs are idempotent and are not entered
in the dedup table.

### Client attempts (rule 5.7)

`Runtime::submit` → up to 5 attempts, each a **new** request identity:

* recompute placement every attempt;
* `NOT_RESPONSIBLE` → next attempt goes to the other owner, after
  `n x 400 ms` backoff (the sum outlasts a membership timeout);
* attempt deadline (3 s, with 3 retransmissions inside it) → retry
  after backoff with fresh placement;
* `STALE_VERSION` → immediate new attempt;
* `STORAGE_ERROR` → retried with backoff (usually a full log while
  compaction runs, or a full dedup table).

Exactly-once holds per request identity. A new attempt after a whole
deadline expired is a new request, so a mutation may apply twice; SET
and DELETE are idempotent in effect (same value / still deleted, with
a newer version).

### Timeout ordering (config.h static_asserts)

```
REPL_DEADLINE 1500 + PRIMARY_BUDGET 500  <  CLIENT_ATTEMPT_DEADLINE 3000
CLIENT_ATTEMPT_DEADLINE 3000  <  DEDUP_EVICT_AFTER 3250  <  DEDUP_RETENTION 6000
DEDUP_IN_PROGRESS_TIMEOUT 3000  >  REPL_DEADLINE + PRIMARY_BUDGET
backoff sum 400*(1+2+3+4) = 4000  >  MEMBERSHIP_TIMEOUT + HEARTBEAT
```

So a primary whose replica died answers REPLICATION_FAILED before the
client gives up the attempt, and a retry always finds its dedup entry.

### Dedup capacity

64 entries holding at most 64-byte acknowledgements. Under pressure the
oldest COMPLETED entry older than 3.25 s (longer than any sender keeps
retransmitting) is reused; IN_PROGRESS entries are reclaimed after 3 s
as a leak guard. If the table is still full the request is answered
`STORAGE_ERROR` (never silence) and the client retries. Sustained
capacity is about 20 mutations/s per node, above what EEPROM write
latency allows anyway.

## 7. Storage

AT24C256 layout (unchanged on-EEPROM format):

```
0x0000  superblock slot 0 (64 B)   generation odd
0x0040  superblock slot 1 (64 B)   generation even
0x0200  bank A: header 32 | records ... | footer 32   (16128 B)
0x4100  bank B: same
```

Records: 22-byte header, key, value, CRC-16/CCITT, commit marker
`A5` written last after a verified write of the body. RAM index of up
to 64 logical keys (48 B each), rebuilt at boot by replaying the active
bank (highest version per key wins).

Recovery distinguishes an interrupted append from corruption: an
undecodable record whose following bytes (beyond one maximum record
length) are erased is a **torn tail**; it is wiped and appends resume
there. Anything else is `CORRUPTED`; the runtime then reformats and the
replicas repopulate the node through anti-entropy
(`config::STORAGE_AUTO_FORMAT_ON_CORRUPTION`).

> The audit listed torn-tail detection as done; in fact the old code
> returned `CORRUPTED` for every torn record (it checked for erased
> bytes starting at the torn record itself). Found by the new power-
> loss tests and fixed.

**Compaction** (crash safe, incremental): when the active bank has
less than 3 KB free (or garbage-collected keys wait and the index is
75 % full) the runtime starts a compaction and advances it one step per
loop: clear the other bank two pages at a time, write its header, copy
the current record of one key per step (a key written again after its
copy is copied again), then footer, sealed header and the superblock
that atomically switches banks. Until that superblock commits the old
bank stays authoritative and keeps taking appends. If the log fills
completely, the write gets `NO_SPACE` → `STORAGE_ERROR` and the client
retries after the compaction.

`test/storage_manager_test.cpp` cuts power at every byte of an append
and every few bytes of a whole compaction and checks every key after
reboot.

EEPROM wear is measured, not estimated: the fake EEPROM counts write
cycles per 64-byte page (one I2C page write = one cycle), and the 2-hour
soak (H13) reports the hottest page. Every append costs two cycles on
its last page (body, then the commit marker), and compaction alternates
banks, so wear is spread over both banks. Result at the default 30 s
sensor period: 28 cycles in 2 h on the hottest page, i.e. about
**8 years** to the AT24C256's 1 M-cycle rating (about 3 years at 10 s).
Lifetime scales linearly with the sample period.

## 8. Anti-entropy and hand-off

Every 10 s (and twice during CATCHUP) each node broadcasts its version
table in VERSION_TABLE chunks of 5 entries (`sync_id`, chunk index,
count). Chunks and pushes are paced 40 ms apart. For each received
entry the receiver compares with its own copy:

* receiver's version newer → queue a RESYNC_DATA push to the sender,
  if the sender owns the key (placement with the sender counted alive);
* receiver's version older or equal → nothing; the sender pushes to us
  when it processes our table;
* once **all chunks** of one `sync_id` arrived, every key we hold that
  the sender did not list and that the sender owns is pushed too.

RESYNC_DATA is applied with LWW and no ownership check. Lost packets
are repaired next round (non-ACKed by design).

This one mechanism covers: an under-replicated write reaching its
replica, a rebooted node catching up (it announces its table in
CATCHUP; peers count it as alive for ownership), hand-off when the ring
view changes (the new replica owns keys it lacks, so it gets pushes),
and a wiped node being rebuilt.

### Garbage collection (decision D8)

From complete tables received within the last 30 s, and only if they
were compared after our last local change of that key:

* **orphan**: we hold a key we no longer own and every current owner
  reported a version >= ours → drop our copy;
* **tombstone**: older than 10 min, all four nodes alive, and every
  other node reported no copy or a version >= ours → drop it.

Drops are physical (`StorageManager::mark_drop`, applied by the next
compaction, which they can trigger under index pressure); no record is
written. The 10-minute age is far beyond any retry or in-flight
replication, so a late REPL_REQ cannot resurrect the key.

## 9. Consistency model

Eventually consistent, last-writer-wins per key, RF = 2:

* a committed write (OK or UNDER_REPLICATED) is durable on at least one
  node and reaches both owners through replication or anti-entropy;
* reads go to the primary and see every write that primary has applied;
  right after a membership change a read may be served by a node that
  has not yet received the latest write — the boot gate and the
  primary-only read path keep this window short (H8 tests that a
  rejoining old primary never serves its stale copy);
* concurrent writers to one key: the higher (HLC, origin) wins
  everywhere;
* not linearizable, no quorum, no transactions (out of scope).

## 10. Sensors (simulated until D1 is answered)

Each node runs a `sensor::Sampler` over a `sensor::Source`. Until the
real parts are known every node uses a deterministic `SimulatedSource`
(A light, B motion, C sound, D temperature; level kinds follow a
20-minute "day" wave, a ripple and a bounded random walk, event kinds
draw a few events per window). A real driver implements `Source`
(`begin`, `kind`, `read`, optionally `poll`) and replaces the simulated
one in `src/platform/main.cpp`; nothing else changes. If a real sensor is on
I2C it must share the EEPROM's bus through a bus owner with a mutex, or
use other pins (see DECISIONS D2).

Every 30 s (`config::SENSOR_PERIOD_MS`, staggered per node; chosen for
EEPROM endurance, see §7) a sample is encoded (20 bytes for level
kinds, 24 for event kinds, see `sensor.h`) and written to `s/<node>/<kind>` and to the history slot
`h/<node>/<kind>/<seq % 4>`. At start-up the sampler reads its own
latest key back from the cluster and continues the sequence number, so
the history ring stays ordered across reboots.

## 11. Pi bridge

UART1 of node A (GPIO20 RX, GPIO21 TX, 115200 8N1). Framing
`AA 55 | LEN | envelope | CRC-8` (CRC-8/SMBUS over LEN + envelope;
check value `F4`). The decoder resynchronises after noise, bad lengths
and bad CRCs, and abandons a partial frame after 100 ms of silence.
The envelope is an ordinary protocol packet; the bridge runs SET/GET/
DEL through the same client API as the console, echoes the Pi's
request id, and answers HEARTBEAT with its node id and HLC.

### Mapping outcomes to frozen codes

| client outcome | status sent to the Pi |
|---|---|
| OK / NOT_FOUND | OK / NOT_FOUND |
| UNDER_REPLICATED | REPLICATION_FAILED (write committed) |
| STALE_VERSION, STORAGE_ERROR, KEY/VALUE_TOO_LARGE, INVALID | same code |
| NOT_READY, BUSY, NO_ROUTE, TIMEOUT | NOT_RESPONSIBLE (could not be served now; retry) |

## 12. Capacity

| item | limit |
|---|---|
| keys per node (incl. tombstones) | 64 |
| key / value | 32 B / 192 B |
| sensor keys in the cluster | 4 x (1 + 4) = 20 (≈ 10 per node with RF = 2) |
| user keys | about 100 cluster-wide, fewer with uneven placement or after failovers (orphans are collected) |
| client ops in flight per node | 8 |
| replications in flight per node | 8 (more → UNDER_REPLICATED, repaired by AE) |
| Pi requests in flight | 4 |
