# Project status

Last updated: 2026-10-17 (at commit `59e2777`).

This file says where the project stands: what is built, what has been
tested and where, and what is left. It is meant for anyone joining the
project, whether a person or an AI agent, so they can start working
without reading the whole history first. The deeper documents are
listed at the end. When this file and the code disagree, the code is
right, and this file should be fixed.

---

## 1. The project in one paragraph

Four ESP32-C3 boards (nodes A, B, C, D), each with an AT24C256 32 KB
EEPROM, together form one key-value store. They talk to each other
over ESP-NOW (no Wi-Fi router, no internet). Every key is stored on two
boards, chosen by consistent hashing. If a board dies, the others keep
serving its keys, and when it comes back it is brought up to date
automatically. Each board also samples a sensor and writes the readings
into the store. A Raspberry Pi is wired to node A over UART and can
read and write the store and pull out the sensor data. The point of the
project is to show real distributed-systems ideas (the ones Dynamo and
Cassandra use) working on cheap hardware.

---

## 2. Where things stand

| area | state |
|---|---|
| Distributed store (firmware logic) | **Done.** All features in, tested in a 4-node simulation |
| Host test suite | **Green.** 30 C++ test programs + 14 Python tests, `make check` passes |
| Firmware build | **Green.** All 8 images (`node-a..d`, `node-a-fi..d-fi`) build. Flash use about 62 %, RAM about 29 % |
| CI (GitHub Actions) | Set up. Runs `make check` and builds every firmware image on each push |
| Raspberry Pi client (`pi/`) | **Done.** CLI + library, tested end to end against the simulator |
| Tests on real hardware, integrated firmware | **Not done yet.** The plan is written (`docs/HARDWARE_TEST_PLAN.md`, H1–H14). No logs collected yet |
| Real sensors | **Not started in code.** Every node runs a *simulated* sensor. The friend is wiring the real ones (DHT11, BH1750, PIR, sound) |
| Pi dashboard for the demo | **Not started.** Fully planned in `docs/DEMO_PLAN.md` |
| Demo script, measured numbers, poster/slides | **Not started.** Also in `docs/DEMO_PLAN.md` |

In short: the hard part (the distributed store itself) is built and
tested in simulation. What is left is (1) proving it on the four real
boards, (2) the real sensors, and (3) the dashboard and material for
the college demo.

---

## 3. How we got here

Taken from the git history, so you can tell what was built on real
boards and what was built on the PC.

| date | what happened | where it was checked |
|---|---|---|
| 2026-09-17 | ESP32-C3 PlatformIO project set up; ESP-NOW between two boards works | real boards |
| 2026-09-21/22 | Binary protocol (`include/protocol.h`, 22-byte header, 11 message types) built and sent over ESP-NOW | real boards + host tests |
| 2026-09-24 | AT24C256 EEPROM driver written | real EEPROM |
| 2026-09-27 | Persistent log storage with compaction | real EEPROM + host tests |
| 2026-10-01 | Phase 4R: request reliability (retries, request IDs, dedup) | host tests, boards |
| 2026-10-06 | Phase 5: heartbeats, membership, consistent-hash placement | host tests, boards |
| 2026-10-09 | GET / SET / DELETE working over the radio | real boards |
| 2026-10-17 | Big integration: node runtime, RF=2 replication, anti-entropy, boot/catch-up gate, garbage collection, sensors (simulated), Pi bridge, console, full PC simulator, whole-cluster tests. All Arduino code moved to `src/platform/` so the rest builds on a PC | **host only** |
| 2026-10-17 | Pi package, docs (DESIGN, DECISIONS, HARDWARE_TEST_PLAN), CI, demo plan | host only |

So the early layers (radio, protocol, EEPROM driver, basic GET/SET/DEL)
have run on real boards. The integrated firmware from 2026-10-17 has
**not** been flashed and tested on the four boards yet. That is the
next real milestone.

The original phase-by-phase plan is in `docs/devise` (informal notes,
phases 1–13). All 13 phases are now done in code.

---

## 4. What exists, folder by folder

```
include/, src/     portable C++17: everything except hardware access
src/platform/      ESP32-only code: radio, EEPROM driver, main.cpp
sim/               PC stand-ins: fake EEPROM, simulated radio, 4-node cluster
test/              host tests, unit level and whole-cluster scenarios
tools/edge_sim.cpp interactive simulator you can type into
pi/                Raspberry Pi Python package (edgekv)
docs/              design notes, decisions, test plan, demo plan, logs
```

The important rule of the layout: nothing under `src/` (except
`src/platform/`) uses Arduino. That is why the same code can run four
times inside one PC process, and why most bugs can be found without
hardware.

### Modules (`include/` + `src/`)

| module | what it does |
|---|---|
| `protocol`, `packet_builder` | Encode/decode the frozen wire format. `protocol.h` is the spec; do not change the header |
| `uart_frame` | Framing for the Pi link: `AA 55 | len | packet | CRC-8` |
| `hlc` | Hybrid logical clock (48-bit ms + 16-bit counter). Every write's version is `(HLC, origin node)` |
| `consistent_hash`, `hash_ring`, `placement` | FNV-1a hash, ring with 64 virtual points per node, picks primary + replica for a key among live nodes |
| `heartbeat`, `membership` | Heartbeat every 500 ms; a node is dead after 2 s of silence. Each node keeps its own view |
| `request_identity`, `request_manager`, `reliable_packet` | Request IDs, timeouts, retransmission with backoff, `FLAG_RETRY` |
| `dedup_manager` | Remembers recent requests and their answers, so a retried write runs once |
| `kv_engine` | Last-writer-wins logic, tombstones for deletes |
| `storage`, `storage_record`, `storage_metadata` | EEPROM log: superblock, two banks (A/B), records with CRC and commit marker, RAM index of 64 keys, incremental crash-safe compaction |
| `node_runtime` | The heart of a node: routing client ops, serving as primary or replica, replication, the BOOTING → CATCHUP → ACTIVE gate. Biggest file (~2000 lines) |
| `anti_entropy` | Every 10 s nodes swap version tables and push missing/newer keys to each other. Also garbage collection of old tombstones and leftover copies |
| `sensor` | `Source` interface, `SimulatedSource`, `Sampler` (writes `s/<node>/<kind>` and a 4-slot history ring `h/<node>/<kind>/<n>`), 20-byte reading format |
| `bridge` | Node A side of the Pi link: turns UART frames into normal client ops |
| `console` | The serial command line on every board (`help` lists commands) |
| `include/config.h` | **Every timing and capacity number lives here.** `static_assert`s check the timeouts stay in the right order |

### Platform code (`src/platform/`)

* `main.cpp`: builds the objects and starts the one node task. The
  sensor source is created here (currently `SimulatedSource`, around
  line 90). Real sensor drivers get plugged in here.
* `transport.cpp`: ESP-NOW. **The four board MAC addresses are
  hard-coded here** (lines ~25–37, A to D in order). Each physical board
  must be flashed with the env matching its MAC.
* `eeprom.cpp`: AT24C256 driver on I2C (SDA GPIO8, SCL GPIO9).
* `platform_time.cpp`: millisecond clock.

The platform is pinned to `espressif32@7.1.3` (Arduino-ESP32 2.0.x /
IDF 4.4) because the ESP-NOW callback signatures changed in IDF 5. The
build stops with `#error` on IDF 5 or newer. Upgrading is a deliberate
job, not a casual bump.

### Simulator and tests

* `sim/cluster.*` runs four real `edge::Runtime`s with fake EEPROMs and
  a fake radio, in virtual time. You can switch nodes off/on, cut
  links, add packet loss, cut power mid-write.
* `test/cluster_test.cpp` has a scenario for each hardware test, plus
  extra ones: `H2 H3 H4 H5 H6 H7H8 H9 H10 RACE H11 H11F H12 SPLIT LOSS
  GC H13`. Run one verbosely with
  `build/host/test/cluster_test H7H8 -v`.
* `test/storage_manager_test.cpp` cuts power at every byte of an
  append and through a full compaction, then checks every key.
* `test/ring_balance_test.cpp` checks the ring spreads keys evenly
  (each node owns 23–27 %).
* `tools/edge_sim.cpp` is the interactive simulator. It also exposes
  node A's bridge on a pseudo-terminal, so the Pi client can be run
  against it on a laptop.

### Raspberry Pi package (`pi/`)

Python 3.8+, only needs `pyserial`.

* `edgekv/framing.py`, `protocol.py`: Python side of the UART framing
  and wire format.
* `edgekv/client.py`: `EdgeKV` class (`set`, `get`, `delete`,
  `sensors`, `history`, `wall_time`).
* `edgekv/reading.py`: decodes the 20-byte sensor reading.
* `edgekv/cli.py`: `python3 -m edgekv ping|set|get|del|sensors|history|watch`.
  `watch --csv file` logs readings to CSV.
* Tests: 6 framing, 7 protocol, 1 end-to-end against the simulator
  (skipped if `make sim` was not run).

---

## 5. How it works (short version)

Details are in `docs/DESIGN.md`. This is enough to find your way around.

**Placement.** A key is hashed onto a ring. The first live node
clockwise is the *primary*, the next different live node is the
*replica*. Every node computes this itself; there is no leader. Example
with all nodes up: `s/A/temp` lives on D (primary) and B (replica), not
on A, which measures it.

**A write.** Any node can take a SET. It sends it to the primary. The
primary gives it a version (its HLC + its id), appends it to EEPROM,
sends it to the replica, waits for the ack, then answers. If the
replica is down or slow (1.5 s), the write is still kept on the primary
and the answer is `UNDER_REPLICATED`. Anti-entropy copies it over later.

**A read.** Goes to the primary only.

**Retries.** Lost packets are resent with the same request ID; the
dedup table makes sure the write still runs once. If a whole attempt
times out the client tries again (up to 5 times), working out placement
again each time.

**Failure.** A node silent for 2 s is treated as dead. Its keys are now
served by the next node on the ring. Only that node's keys move.

**Rejoin.** A rebooted node stays silent first (BOOTING, 2.5 s), then
announces what it has (CATCHUP, 3 s) while the others push what it is
missing, and only then starts serving (ACTIVE). So it never serves
stale data.

**Anti-entropy.** Every 10 s, each node broadcasts its version table.
Whoever has a newer copy of a key the sender owns pushes it. This one
mechanism fixes under-replicated writes, catches up rebooted nodes,
moves data after a ring change, and rebuilds a wiped node.

**Storage.** Append-only log in EEPROM, two banks. When the active bank
is nearly full it is compacted into the other bank a step at a time;
a superblock switch makes it atomic. A torn write from a power cut is
detected and thrown away on boot.

**Sensors.** Every 30 s each node writes its reading to the latest key
`s/<node>/<kind>` and one of 4 history slots `h/<node>/<kind>/<seq % 4>`.
These are normal SETs through the same path as everything else.

**Pi.** Talks to node A over UART1 (GPIO20 RX, GPIO21 TX, 115200). Node
A turns each frame into a normal client operation.

---

## 6. What has been verified, and where

### On the PC (done 2026-10-17, re-run while writing this file)

* `make check`: 30/30 C++ test programs pass, 14/14 Python tests pass.
* Every hardware scenario H2–H13 passes in simulation, plus split brain,
  lossy radio (packet loss), garbage collection and a write race.
* 2-hour simulated soak (H13): 5 reboots, 1918 sensor writes OK, 0
  failed, 18 compactions, 0 storage errors. Hottest EEPROM page took 28
  write cycles in 2 h, which works out to about 8 years of life at the
  30 s sample period.
* `make firmware`: all 8 images build.
* Saved output: `docs/test-logs/host-tests.log`.

One note: the very first `make -j8 check` run while writing this file
exited with code 1 even though every test printed PASS. Three clean
`make -j8 check` runs and one serial `make check` afterwards all passed.
It could not be reproduced. If you see it again, run `make check`
without `-j` and look at the log; it may be a timing-sensitive test or
a parallel build race.

### On real hardware

* Done earlier (before the integration): ESP-NOW between boards,
  protocol over ESP-NOW, EEPROM driver, storage compaction on the real
  chip, GET/SET/DELETE over the radio.
* **Not done:** anything in `docs/HARDWARE_TEST_PLAN.md` with the
  integrated firmware. `docs/test-logs/` has no hardware logs yet. A
  hardware test only counts as passed when its serial log is saved
  there.

The simulation does not have real EEPROM timing, radio interference or
USB/UART quirks, so passing on the PC does not replace the hardware
run.

---

## 7. What needs to be done

In a sensible order. Steps A and B can run alongside each other.

### A. Run the hardware test plan (H1–H14)

Who: the owner, at the bench. Guide: `docs/HARDWARE_TEST_PLAN.md`.

1. Wire an AT24C256 to each board (GPIO8/9, address 0x50, WP to GND).
2. Check the MAC table in `src/platform/transport.cpp` matches the
   four boards; flash `node-a..d`.
3. Pre-flight: every board `ACTIVE`, view `ABCD`.
4. Go through H1–H14. H4, H5, H10 need the `-fi` images. H13 is a
   2-hour soak. H14 needs the Pi wired to node A.
5. Save the logs in `docs/test-logs/` with the names given in the plan.
6. Afterwards, tune `include/config.h` from the measured numbers
   (latency from `bench 200`, view flapping, radio failures). The
   `static_assert`s will catch timeouts set in the wrong order.

Things that may bite on hardware:

* GPIO20/21 also carry the ROM bootloader's text at reset. The Pi sees
  some garbage when node A resets; the decoder skips it. If a board
  routes these pins to a USB-UART chip, move the bridge pins in
  `config.h`.
* TX power is set to 8.5 dBm in `transport.cpp`. If boards are far
  apart and `stats` shows many `failed` sends, look there.
* Nothing below `src/platform/` has been run on a board since the
  refactor, so expect small integration bugs (stack size, task timing,
  serial output). The node task has a 16 KB stack; `stats` prints the
  headroom.

### B. Real sensors (DEMO_PLAN phase 1)

Who: the friend does the drivers and wiring; the core changes are
small and listed in `docs/DEMO_PLAN.md`.

Chosen parts and mapping:

| node | part | kinds | pin |
|---|---|---|---|
| A (bridge) | DHT11 | temperature, humidity | GPIO4 |
| B | BH1750 | light | I2C, shared with the EEPROM (0x23 vs 0x50) |
| C | AM312 PIR | motion (new kind) | GPIO4 |
| D | sound module (digital out) | sound (new kind) | GPIO4 |

Avoid GPIO2/8/9 (strapping/I2C), 18/19 (USB) and 20/21 (bridge UART on A).

Note this replaces the current simulated mapping (A temp, B humidity,
C pressure, D light). Pressure goes away, motion and sound are new.

Work to do:

* `include/sensor.h` / `src/sensor.cpp`: add `MOTION = 5`, `SOUND = 6`;
  replace `kind_for_node` with a per-node list of sources (node A has
  two).
* An `EventSource` for PIR/sound that counts triggers per period
  instead of writing on every trigger (to save the EEPROM), plus an
  early write when activity starts, at most one per 10 s.
* Drivers in `src/platform/sensors_*.cpp` implementing `sensor::Source`
  (`begin`, `kind`, `simulated()` returning false, `read`). Use pinned
  PlatformIO libraries. Keep `SimulatedSource` as a fallback if
  `begin()` fails, so a loose wire does not kill the demo.
* `config::SENSOR_HISTORY_SLOTS` 4 → 8 (gives 4 minutes of back-fill;
  EEPROM life drops to about 6 years; re-check with H13).
* `pi/edgekv/reading.py`: new kinds and units.
* Tests: extend `test/sensor_test.cpp` and the sensor part of
  `cluster_test`.

The I2C sharing rule (DECISIONS D2): only the node task may touch I2C.
That is already the case if the BH1750 driver is called from
`Source::read`. If a library wants its own task or interrupts, a mutex
is needed around the EEPROM and the sensor.

### C. Observability command (DEMO_PLAN phase 2)

Add an `obs` console command printing one machine-readable line per
node (state, view, HLC, keys with versions), so the dashboard shows what
is really stored on each board. Small change in `src/console.cpp`, with
a test in `test/console_test.cpp`.

### D. Pi dashboard (DEMO_PLAN phase 3)

The centrepiece of the demo. Python standard library only
(`http.server` + Server-Sent Events) and one HTML page with inline SVG,
no CDN, so it works on a college network with no internet. Run as
`python3 -m edgekv dashboard`, with a `--sim` mode that drives
`edge_sim` so the whole demo can be rehearsed on a laptop.

Needs: a Python port of the ring (`pi/edgekv/ring.py`) pinned to the
C++ one by test vectors; a SQLite archive (`pi/edgekv/archive.py`) that
back-fills gaps from the history slots; views for the ring, a key
explorer, a "what moved" panel (keys moved by consistent hashing vs.
what `hash mod N` would move), node cards and sensor charts.

### E. Demo, numbers, story (DEMO_PLAN phase 4)

* `docs/DEMO.md`: a 6–7 minute script (pull a board's USB, watch the
  keys move and come back, unplug the Pi and watch the chart back-fill).
* Measured results from hardware: SET latency p50/p95, failover time,
  rejoin time, keys moved per failure, packet loss tolerance, EEPROM
  wear. A results table and a GIF in the README.
* A poster or about 5 slides.

### Small housekeeping

* `docs/devise` is listed in `.gitignore` as local notes but is still
  tracked by git. Either `git rm --cached docs/devise` or remove it from
  `.gitignore`. Decide which.
* The "master architecture v3" document mentioned in the early notes is
  not in the repo (DECISIONS D0). If it turns up, add it under `docs/`
  and reconcile any differences.
* The README "Status" line should be updated once hardware tests pass.

---

## 8. Limits (by design, not bugs)

* Eventually consistent, last-writer-wins per key. Not linearizable, no
  quorum reads or writes, no transactions.
* 64 keys per node, tombstones included. Keys up to 32 bytes, values up
  to 192 bytes. Roughly 100 user keys cluster-wide on top of the sensor
  keys.
* Exactly four nodes, fixed MAC table, one radio hop. No dynamic
  membership.
* A write retried after a full attempt timeout is a new request and may
  apply twice. SET and DELETE are idempotent, so the result is the same.
* The store is a short-term buffer at the edge, not an archive. Sensor
  history is a small ring of slots that get overwritten. Long-term
  storage is meant to happen on the Pi (CSV now, SQLite planned).
  Keys like `temp_<timestamp>` would fill the cluster in about 15
  minutes; DEMO_PLAN explains why.

---

## 9. Commands you will actually use

```bash
# PC
make check                               # everything; must stay green
make test                                # C++ tests only
build/host/test/cluster_test H6 -v       # one cluster scenario, verbose
make sim && build/host/edge_sim          # interactive 4-node simulator
#   then type:  A: set k v   B: get k   off C   on C   cut A B   heal   loss 0.2
cd pi && python3 -m edgekv --port /dev/pts/N sensors   # Pi client vs simulator

# Firmware
make firmware                            # build all 8 images
pio run -e node-b -t upload --upload-port /dev/ttyACM1
pio device monitor -p /dev/ttyACM1 -b 115200

# On a board (serial console)
help  status  members  stats  dump  ring <key>  set/get/del  sensors  bench 100

# On the Pi
python3 -m edgekv --port /dev/serial0 ping
python3 -m edgekv --port /dev/serial0 watch --interval 30 --csv readings.csv
```

---

## 10. Ground rules for anyone changing the code

These are the rules the code was built around. Break one only on
purpose, and write down why in `docs/DECISIONS.md`.

1. **The wire protocol is frozen.** Do not change the header or the
   message/status tables in `include/protocol.h`. The Pi client
   depends on them too.
2. Only `StorageManager` touches the EEPROM.
3. Protocol code knows nothing about ESP-NOW or EEPROM.
4. The ESP-NOW receive callback only copies into a queue.
5. One retry system for everything (client requests and replication).
6. Every node computes placement itself.
7. Sensor writes go through the normal SET path.
8. Timing and capacity numbers go in `include/config.h`, nowhere else.
9. One FreeRTOS task owns all node state. No locks, no second task
   touching the runtime.
10. `make check` stays green. New behaviour gets a host test, and if it
    can be, a cluster scenario. Code under `src/` (not `src/platform/`)
    must keep building on a PC with no Arduino headers.
11. A design call that someone could reasonably disagree with goes
    into `docs/DECISIONS.md` (next free number is D9).
12. Hardware test results only count with a saved log in
    `docs/test-logs/`.

---

## 11. Which document to read for what

| file | read it when |
|---|---|
| `README.md` | first look, build commands, console reference |
| `STATUS.md` (this file) | you want to know what is done and what is next |
| `docs/DESIGN.md` | you need to understand or change how something works |
| `docs/DECISIONS.md` | you wonder why something is the way it is (D0–D8) |
| `docs/HARDWARE_TEST_PLAN.md` | you are at the bench with the boards |
| `docs/DEMO_PLAN.md` | you work on sensors, the dashboard or the demo |
| `pi/README.md` | you set up the Raspberry Pi |
| `docs/test-logs/` | you want evidence of what passed |
| `docs/devise` | old informal phase plan, history only |
| `docs/archive/` | old code kept for reference, not built |
