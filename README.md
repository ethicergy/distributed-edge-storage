# edge-kv — a distributed key-value store on four ESP32-C3 boards

Four ESP32-C3 nodes, each with an AT24C256 EEPROM, form a leaderless,
replicated key-value store over ESP-NOW. Keys are placed with consistent
hashing (replication factor 2), every write is versioned with a hybrid
logical clock, requests are retried with exactly-once semantics, and
anti-entropy repairs whatever a failure left behind. Each node samples a
sensor into the store; a Raspberry Pi reads and writes through a UART
bridge.

```
        Raspberry Pi  (python3 -m edgekv ...)
             │ UART 115200  AA 55 | len | packet | CRC-8
   ┌─────────┴──┐   ESP-NOW ch 6    ┌────────────┐
   │  node A    │◄─────────────────►│  node B    │
   │  bridge    │◄───┐        ┌────►│            │
   └─────┬──────┘    │        │     └─────┬──────┘
     EEPROM          ▼        ▼         EEPROM
   ┌────────────┐                   ┌────────────┐
   │  node C    │◄─────────────────►│  node D    │
   └─────┬──────┘                   └─────┬──────┘
     EEPROM                             EEPROM
```

**Status:** feature complete on the host: every scenario of the
hardware test matrix passes in the 4-node simulation, the firmware
builds for all nodes, and the Pi client passes end to end against the
simulator. Sensors are simulated until the real parts are chosen
(`docs/DECISIONS.md`, D1). On-hardware validation follows
[docs/HARDWARE_TEST_PLAN.md](docs/HARDWARE_TEST_PLAN.md).

## Features

* Symmetric nodes: one firmware image, behaviour decided only by
  placement over each node's own membership view.
* Consistent-hash ring (64 virtual points per node), primary + replica,
  automatic failover and hand-back.
* Hybrid logical clock versions (HLC, origin), last-writer-wins,
  tombstones; clocks converge through every packet.
* Request manager (timeouts, backoff, FLAG_RETRY) and dedup manager
  (cached responses): a retried or duplicated write executes once.
* RF = 2 replication through the same reliability machinery; defined
  outcome when the replica is down (committed, under-replicated).
* Boot / catch-up gate: a rejoining node never serves stale data.
* Push-based anti-entropy (VERSION_TABLE / RESYNC_DATA) for repair,
  hand-off and rebuilding a wiped node; garbage collection of
  tombstones and orphan copies.
* Crash-safe EEPROM log with A/B banks and incremental compaction;
  tested with power loss at every byte.
* Serial console on every node, Pi bridge with a Python client and CSV
  logging, and a PC simulator of the whole cluster.

## Repository

```
include/, src/        platform-independent C++17 (protocol, storage, HLC,
                      placement, runtime, anti-entropy, sensors, bridge, console)
src/platform/         ESP32 only: ESP-NOW transport, AT24C256 driver, main.cpp
sim/                  host simulation: fake EEPROM, simulated radio, 4-node cluster
test/                 host tests (unit + whole-cluster scenarios)
tools/edge_sim.cpp    interactive simulator with the Pi bridge on a pty
pi/                   Raspberry Pi package and CLI (python3 -m edgekv)
docs/                 DESIGN, DECISIONS, HARDWARE_TEST_PLAN, test logs
include/config.h      every timing and capacity parameter
```

## Build and test

Host (Linux/macOS, g++ or clang with C++17, Python 3.8+):

```bash
make check        # 30 C++ test programs + simulator + Python tests
```

`make test` runs only the C++ tests; one cluster scenario verbosely:
`build/host/test/cluster_test H7H8 -v`.

Firmware (PlatformIO; the platform version is pinned):

```bash
make firmware                                   # all images
pio run -e node-b -t upload --upload-port /dev/ttyACM1
pio device monitor -p /dev/ttyACM1 -b 115200
```

`node-a` … `node-d` are the normal images; `node-a-fi` … `node-d-fi`
add fault-injection console commands for the reliability tests. Node A
is the Pi bridge (`EDGE_BRIDGE_NODE_ID` in `platformio.ini`).

## Try it without hardware

```bash
make sim
build/host/edge_sim            # prints the pty of node A's bridge
```

Type console commands (`A: set k v`, `B: get k`, `sensors`, `status`),
cause failures (`off C`, `on C`, `cut A B`, `heal`, `loss 0.2`), and in
another terminal use the Pi client on the printed pty:

```bash
cd pi && python3 -m edgekv --port /dev/pts/N sensors
```

## Console

On each board (USB, 115200 baud), `help` lists:

| command | |
|---|---|
| `status`, `members`, `stats` | node state, view, storage, counters |
| `set <k> <v>`, `get <k>`, `del <k>` | distributed operations from this node |
| `ring <k>` | primary/replica of a key |
| `sensors`, `sensor on\|off\|period <ms>` | read all sensors / control this node's sampler |
| `dump` | this node's local keys with versions |
| `sync`, `compact`, `format yes`, `reboot` | operator actions |
| `bench <n>` | n sequential SETs with latency summary |
| `log off\|info\|debug` | log verbosity |
| `fault ...`, `stale <k> <node>` | fault injection (`-fi` images) |

## Raspberry Pi

See [pi/README.md](pi/README.md). Wiring: Pi GPIO14 (TXD) → node A
GPIO20, Pi GPIO15 (RXD) ← node A GPIO21, common ground.

```bash
python3 -m edgekv --port /dev/serial0 sensors
python3 -m edgekv --port /dev/serial0 watch --interval 30 --csv readings.csv
```

## Known limits

* Eventually consistent, last-writer-wins per key; not linearizable, no
  quorum reads/writes.
* 64 keys per node including tombstones (garbage-collected after 10 min
  with all nodes confirming); keys ≤ 32 bytes, values ≤ 192 bytes.
* Static four-node membership (fixed MAC table), single radio hop.
* EEPROM endurance is about 8 years at the default 30 s sensor period
  (measured in the soak test; it scales with the period).

More in [docs/DESIGN.md](docs/DESIGN.md).

## License

MIT — see [LICENSE](LICENSE).
