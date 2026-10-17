# edge-kv: from "working" to a convincing college demo

## Context

The distributed store is finished and tested on the host (30 C++ + 14 Pi tests,
CI green). A friend is wiring the real sensors: **DHT11** (temp + humidity),
**BH1750** (light, I2C), **AM312 PIR** (motion), **sound module** (digital
out). The goal is a college evaluation demo where people *see* why this is a
real distributed-systems project (Dynamo/Cassandra ideas on four ₹300
boards), especially **consistent hashing**, without buying anything and
without rewriting the project. Everything below is additive: new kinds,
drivers, one console command, and a Pi dashboard.

Two confusions to settle first (also go into README/docs):

**1. Where does the temperature data live?** It does *not* live on the node
that measures it. The key name decides. `s/A/temp` hashes to the ring → with
all four nodes up its owners are **primary D, replica B** (checked with the
simulator's `ring` command). Node A measures it and then sends it over
ESP-NOW to D, which copies it to B. Each key has a fixed pair of owners
while membership is stable. The history slots `h/A/temp/0..3` are
different keys, so they land on other pairs (A+C, C+A): one sensor's data
is spread over the cluster. If D dies, the next live node clockwise takes
over D's keys. Only D's keys move. That is the property to show visually.

**2. Why not keys like `temp_<timestamp>`?** Each sample would be a new key.
Each node indexes 64 keys and has 32 KB of EEPROM, so at one sample per
30 s the cluster would be full in about 15 minutes. Every old key would
also need a delete (tombstone) and garbage collection. A continuous feed
doesn't fit in 4×32 KB, and it shouldn't. The right model (and the one
already built) is a **ring buffer**: a fixed set of slot keys
`h/<node>/<kind>/<seq % N>` that are overwritten in turn. The timestamp
(HLC) and the sequence number are inside the 20-byte value. The cluster is a
**fault-tolerant short-term buffer at the edge**. The Pi drains it into
permanent storage (SQLite) and can **back-fill gaps** from the ring after it
was disconnected. This "store-and-forward" pattern is what real edge/IoT
systems do, and it is the story of the project.

## Plan

### Phase 1: Real sensors (friend leads, small firmware changes)

Node mapping (one image, chosen by `NODE_ID`):

| node | sensor | kinds | wiring |
|---|---|---|---|
| A (bridge) | DHT11 | TEMPERATURE, HUMIDITY | data → GPIO4 |
| B | BH1750 | LIGHT | shares EEPROM I2C bus GPIO8/9 (addr 0x23 vs EEPROM 0x50) |
| C | AM312 PIR | MOTION (new) | out → GPIO4 |
| D | sound module | SOUND (new) | DO → GPIO4 |

Avoid GPIO2/8/9 (strapping/I2C), 18/19 (USB) and 20/21 (bridge UART on A).

- `include/sensor.h`, `src/sensor.cpp`: add `Kind::MOTION = 5`, `SOUND = 6`
  (key names `motion`, `sound`, unit `events`). Replace `kind_for_node` with
  a per-node source list.
- **Event sensors don't write per trigger** (EEPROM wear). Add an
  `EventSource` that counts edges in each period and reports the count as
  the reading value. Also write early when activity starts after a quiet
  period, rate-limited to one write per 10 s, so a hand-wave shows up on the
  dashboard within seconds.
- Node A has two kinds: run two `sensor::Sampler`s with staggered phases
  (keys already include the kind, so nothing else changes).
- Drivers in `src/platform/sensors_*.cpp` implementing `sensor::Source`
  (`begin/kind/simulated()=false/read`). DHT11 via a pinned PlatformIO lib
  (Adafruit DHT). BH1750 via a pinned BH1750 lib on the existing `Wire`; it
  is safe because only the node task touches I2C (DECISIONS D2, option 1).
  Construct them in `src/platform/main.cpp` in place of `SimulatedSource`
  (line ~90). Keep `SimulatedSource` as a fallback when `begin()` fails, so
  a loose wire never stops a demo.
- `include/config.h`: `SENSOR_HISTORY_SLOTS` 4 → **8**. Capacity: 5 streams ×
  9 keys × RF2 / 4 nodes ≈ 23 keys per node on average, ~30 worst case, out
  of 64. Buffer depth 8 × 30 s = 4 min of back-fill. EEPROM life drops from
  ~8 to about 6 years; re-measure with soak test H13.
- `pi/edgekv/reading.py`: new kinds and units.
- Tests: extend `test/sensor_test.cpp` (event counting, rate limit,
  two samplers on one node, key names). Extend `cluster_test` sensor
  scenario to 5 streams.

### Phase 2: Observability hook (tiny firmware change)

The dashboard must show what is *really* stored on each board, not just
what the math predicts.

- `src/console.cpp`: add an `obs` command that prints one machine-readable
  line: `#OBS node=C state=ACTIVE view=ABCD hlc=… keys=s/A/temp@<ver>,…`.
  It reuses the data behind `cmd_members` / `cmd_dump` (lines ~316, ~564).
  Test in `test/console_test.cpp`.
- On hardware, all four boards plug into the Pi's 4 USB ports (power +
  console). **Pulling a board's USB cable is the failure demo.** Node A
  stays on the UART bridge for data, as designed.

### Phase 3: Pi dashboard (the centrepiece), `pi/edgekv/dashboard/`

Python stdlib only (`http.server` + Server-Sent Events) and one HTML file
with inline SVG and vanilla JS, **no CDN**, so it works on a college network
with no internet. Run it with `python3 -m edgekv dashboard`.

- **Ring port** `pi/edgekv/ring.py`: Python copy of FNV-1a + virtual-point
  ring + primary/replica walk from `src/consistent_hash.cpp` /
  `src/placement.cpp`. Pin it to C++ with test vectors (a C++ test prints
  key → owners for ~50 keys and alive masks; a Python test checks them).
- **Two backends, one interface**:
  - hardware: bridge UART (reads/writes) + 4 USB consoles polled with
    `obs` every ~1 s;
  - simulator: start `build/host/edge_sim` as a subprocess, send
    `A: obs` etc. on stdin and use its bridge pty. It also exposes
    `off C / on C / loss 0.2` as buttons, so the whole demo can be
    rehearsed on a laptop with no boards.
- **Views**:
  1. **Ring**: circle with 4×64 virtual points coloured by node. Every stored
     key is a dot at its hash angle, with lines to its primary and replica.
     Dead nodes go grey.
  2. **Key explorer**: type any key → its hash, its point on the ring and its
     two owners light up. "Store it" does a SET through the bridge, and the
     key then appears in exactly those two boards' `obs` lists.
  3. **"What moved" panel**: on any membership change, count keys whose
     owners changed vs. what `hash mod N` would have moved for the same keys
     (e.g. "6 of 27 moved; mod-N would move 20"). **This is the
     consistent-hashing argument in one number.**
  4. **Node cards**: state (BOOTING → CATCHUP → ACTIVE), key count, log
     usage, counters, with a live event log ("C dead", "anti-entropy pushed
     4 keys to C", "C ACTIVE").
  5. **Sensor charts**: 5 live series from the SQLite archive, plus motion and
     sound events as markers.
- **Archive** `pi/edgekv/archive.py`: SQLite table unique on
  (node, kind, seq). Each period, read the latest key. When the sequence
  number jumped, read the missing history slots. Gaps longer than the ring
  depth are drawn as real gaps (honest), not interpolated.
- Tests: ring vectors; archive dedupe and back-fill against the simulator
  (extends `pi/tests/test_sim_integration.py`).

### Phase 4: Demo, numbers, story

- **Demo script** `docs/DEMO.md` (6–7 min, rehearsed in sim mode):
  1. Live dashboard: four boards, five sensor feeds. "Where does A's
     temperature live?" → D and B.
  2. Wave a hand at C / clap at D → event appears within seconds; the
     explorer shows which boards stored it.
  3. Type the evaluator's name as a key → watch it land on two boards.
  4. **Pull C's USB.** Within ~2 s C greys out and C's keys move to the next
     node; "what moved" shows ~1/4 vs mod-N's ~3/4. Sensor feeds don't
     stop (writes flagged under-replicated).
  5. Plug C back: BOOTING → CATCHUP → ACTIVE and anti-entropy refills it.
     The keys move back.
  6. Unplug the Pi↔A UART for 2 min, reconnect: the chart back-fills with
     no gap (store-and-forward).
- **Measured results** (extend `docs/HARDWARE_TEST_PLAN.md`, logs in
  `docs/test-logs/`): SET latency p50/p95 (`bench 100`), failover time,
  rejoin/catch-up time, keys moved per failure, packet loss tolerance,
  EEPROM wear projection. Put a results table + GIF of the ring demo in the
  README.
- **One-page poster / 5 slides**: problem (edge data without cloud) →
  architecture diagram → consistent hashing + RF2 → failure demo →
  numbers → limits (eventual consistency, 4 nodes, static membership:
  stating limits honestly impresses evaluators).

### Explicitly not doing

No protocol changes (header stays frozen), no rewrite, no new hardware, no
cloud, no dynamic membership, no quorum reads.

## Who does what

- Friend: Phase 1 drivers + wiring (with the event-source/config changes
  done here, so they only write `Source` drivers).
- Claude: Phase 1 core changes, Phase 2, Phase 3, Phase 4 docs. The user and
  friend run the hardware measurements and record the demo GIF.

## Verification

- `make check` green after every phase (new sensor, console, ring-vector
  and archive tests included).
- Sim mode end to end: `python3 -m edgekv dashboard --sim` → run the whole
  `docs/DEMO.md` script with buttons (off C / on C), confirm the ring, the
  "what moved" numbers, the node state transitions and the archive back-fill.
- Hardware (user/friend): flash `node-a..d`, each board shows real readings
  (`sensors` console command, `simulated` flag = 0). Run DEMO.md for real,
  capture serial logs and dashboard screenshots into `docs/test-logs/`.
