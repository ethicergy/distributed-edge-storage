# Decisions

The project audit listed decisions only the project owner could
make. The owner delegated all of them; this file records
what was decided and why, so each one can be revisited deliberately.

| # | Decision | Summary |
|---|----------|---------|
| D0 | Spec document | `include/protocol.h` + `docs/DESIGN.md` are the spec |
| D1 | Sensors | simulated sources behind `sensor::Source` until real parts are chosen |
| D2 | I2C sharing | no I2C sensor yet; rule for when one is added |
| D3 | Data model | latest key + 4-slot history ring per node, 20-byte (level) / 24-byte (event) big-endian reading |
| D4 | Timestamps | HLC in the reading; the Pi converts with the bridge's clock offset |
| D5 | Replica failure | write committed on the primary, reported REPLICATION_FAILED / UNDER_REPLICATED, repaired by anti-entropy; accepted with one live node |
| D6 | Reads after rejoin | primary-only reads behind a boot/catch-up gate |
| D7 | Bridge | node A, UART1 GPIO20/21, Python CLI with CSV logging |
| D8 | Tombstone GC | yes: after 10 min and confirmation from all four nodes; orphan copies too |
| D9 | EEPROM bring-up failure | no flash-backed fallback, I2C clock stays 400 kHz: the cause was wiring |
| D10 | Node B's EEPROM supply | ~~workaround: VCC from GPIO6~~ **superseded Tue 20**: B resoldered, VCC on 3V3 like every node; the GPIO6 workaround was never used on the board |

---

## D0 — Specification

The "master architecture v3" document named in the early planning notes is not in
the repository. The frozen protocol is therefore `include/protocol.h`
(unchanged), and system behaviour is specified by `docs/DESIGN.md`
together with the tests that enforce it. If the original document
turns up, add it under `docs/` and reconcile any difference explicitly.

## D1 — The four sensors

The parts are not described anywhere. Instead of inventing hardware,
every node runs a `sensor::SimulatedSource`:

| node | real part | kind | unit | simulated range |
|---|---|---|---|---|
| A | BH1750 (GY-302) | light | lux | 320 ± 330 |
| B | AM312 PIR | motion | events per window (+ s since last motion) | 0–3 events |
| C | LM393 sound (DO) | sound | events per window (+ % of window active) | 0–6 events |
| D | DHT11 | temperature | °C | 22.5 ± 3.5 |

(Revised 2026-10-17: the parts were chosen; humidity and pressure
were the earlier placeholders for B and C. The bridge node A gets the
BH1750 because its driver never disables interrupts; the DHT11 library
disables them for ~5 ms per read, so it is on D.)

Sample period 30 s (`config::SENSOR_PERIOD_MS`). Readings carry a
`simulated` flag so simulated data can never be mistaken for real
measurements.

**To add a real sensor:** implement `sensor::Source` (`begin`, `kind`,
`simulated() -> false`, `read(now, value_milli)`) and construct it
instead of `SimulatedSource` in `src/platform/main.cpp`. If the kind
differs from the table above, add it to `sensor::Kind` and to
`pi/edgekv/reading.py`. Nothing else changes: the sampler, key schema,
replication and the Pi client stay as they are.

## D2 — Sensors on I2C

There is only one hardware I2C controller on the ESP32-C3 and the
EEPROM uses it (SDA GPIO8, SCL GPIO9). Node D's BH1750 (0x23) now
shares it. Every transfer on the bus holds `i2c_bus::Lock`
(`src/platform/i2c_bus.h`); an EEPROM write keeps it through its write
cycle. The original rule for adding a device was:

* preferred: put it on the same bus. The node task is the only task
  that touches either device, so no mutex is needed as long as the
  sensor driver is called from `Source::read` (it is). Amend R1 to
  "only the node task drives I2C".
* if a driver needs its own task or interrupt-driven transfers, add a
  bus-owner mutex around `eeprom::AT24C256` and the driver.

## D3 — Sensor data model

Option (a) + (b) from the prompt:

* `s/<node>/<kind>` holds the latest reading (overwritten);
* `h/<node>/<kind>/<seq % 4>` is a 4-slot history ring.

That is 5 keys per sensor, 20 in the cluster, about 10 per node with
RF = 2: well inside the 64-key budget, leaving room for user keys.

Reading encoding, big-endian like the protocol. v1 (20 bytes) for
level kinds, v2 (24 bytes) for event kinds:

```
0 format (1 or 2) | 1 kind | 2 flags (bit0 simulated, bit1 settling, bit2 stale, bit3 error) | 3 reserved 0
4..7  sequence  (u32)
8..11 value in thousandths of the unit (i32)
12..19 HLC of the sample (u64)
20..23 v2 only: aux (i32): motion = s from last detection to sample
       (-1: none since boot); sound = milli-% of the window active
```

The sequence survives reboots: at start-up the sampler reads its own
latest key back from the cluster and continues from it.

## D4 — Timestamps

Nodes have no wall clock. Each reading carries the HLC of its sample;
the HLC's physical part is a cluster-wide millisecond clock (all nodes
adopt the most advanced one, see DESIGN §5). Every bridge answer carries
the bridge's current HLC, from which the Pi computes
`offset = pi_wall_ms - hlc_physical_ms` and converts sample times
(`EdgeKV.wall_time`). Accuracy is the UART round trip plus clock drift
since the sample: well under a second. No protocol change was needed.

## D5 — Writes when the replica fails, or no second node is alive

The primary writes locally first, then replicates. If the replica
answers an error or does not answer within 1.5 s, or no other node is
alive, the client gets **REPLICATION_FAILED**, defined as:

> the write is committed and durable on the primary; anti-entropy will
> copy it to the replica when one is reachable.

The client API reports it as `UNDER_REPLICATED`, a committed write;
the Pi CLI prints "OK (under-replicated)". There is no rollback (no
earlier state to roll back to in an LWW append log), and refusing
writes with one live node would make the store unusable exactly when a
sensor most needs to record. If the replica answers STALE_VERSION the
write lost to a newer version and the client is told so (it retries
with a fresh version), see DESIGN §6.

## D6 — Reads after a rejoin

Primary-only reads, protected by a gate: a rebooting node stays silent
(no heartbeats, so nobody routes to it) for one membership timeout,
then announces its version table while peers push everything it owns
and lacks, and only then heartbeats and serves. Reading both owners and
taking the newer version was rejected: GET_RESP carries no version, and
the frozen protocol has no field for one.

Verified by `cluster_test H7H8`: the old primary rejoins holding a stale
value while a client reads continuously; the stale value is never
returned.

## D7 — Raspberry Pi bridge

* Bridge = node A (`EDGE_BRIDGE_NODE_ID` in `platformio.ini`), UART1 on
  GPIO20 (RX) / GPIO21 (TX), 115200 8N1.
* Pi software: Python 3 + pyserial (`pi/`): library `edgekv.EdgeKV`
  and CLI `python3 -m edgekv` with `ping`, `set`, `get`, `del`,
  `sensors`, `history`, `watch [--csv FILE]` (logging to CSV).
* A dashboard was not built: the CSV log feeds any plotting tool, and
  the CLI covers the acceptance test (H14).

## D8 — Garbage collection

Implemented, conservatively:

* **Tombstones** are dropped only when at least 10 minutes old (HLC
  time), all four nodes are alive, and every other node's latest
  complete version table shows no copy or a version at least as new.
  10 minutes is far beyond any retry or in-flight replication, so
  nothing can bring the key back.
* **Orphan copies** (a key this node held while standing in for a dead
  owner) are dropped once both current owners report a version at least
  as new.

Both are local, physical drops applied by the next compaction; no
record is written. Without this the 64-key index would fill
permanently with tombstones and orphans (audit 5.9).

## D9 — No storage fallback, no lower I2C clock

On the first board the AT24C256 did not answer at any address
(BRINGUP_LOG problem 4). Two code changes were considered:

* **Flash-backed fallback:** keep the store in the ESP32-C3's own
  flash when the EEPROM is missing, instead of halting.
* **Lower I2C clock** (100 kHz instead of 400 kHz) in case of a
  marginal bus.

Neither was needed. After rewiring by the module silkscreen every
module answers at 0x50 (and 0x58, its ID block) with the 9.84 kΩ
pull-ups fitted on the modules. The scan that diagnosed it already ran
at 100 kHz and still saw nothing, so the clock was never the cause.
Not adding a fallback keeps the measured EEPROM behaviour (write
cycles, wear, crash safety) the only storage path, and a missing
EEPROM stays a loud FATAL instead of silently degrading. Revisit only
if a module fails during the demo window and no spare is left.

## D10 — Node B's EEPROM powered from GPIO6 (workaround)

**Superseded Tue 20 Oct.** B's 3V3 and GND header pins were resoldered and
its EEPROM is wired to 3V3 like the other nodes. The flag was removed
from `node-b`/`node-b-fi`; `scripts/check_images.sh` now checks that no
image drives an EEPROM power pin. The support code stays, unused
(`config::EEPROM_POWER_PIN` defaults to none). The workaround never ran on
a board: B's GND pin turned out to be loose too (Mon 09:22), so it waited
for soldering instead. The original decision follows.

Board B's 3V3 header pin is loose and there is no soldering iron on
site (BRINGUP_LOG problem 6). Without B the cluster runs on three
nodes, which changes the demo, the measurements and the sensor plan.
Instead, B's EEPROM takes its VCC from GPIO6:

* `-DEDGE_EEPROM_POWER_PIN=6` only in `node-b` and `node-b-fi`
  (`platformio.ini`); every other image keeps VCC on 3V3.
  `scripts/check_images.sh` checks this on the banner text.
* `setup()` drives the pin high at the strongest drive setting
  (`GPIO_DRIVE_CAP_3`), waits `EEPROM_POWER_SETTLE_MS` = 10 ms, then
  starts I2C. Nothing reconfigures the pin afterwards.
  `config.h` refuses, at compile time, a pin that is a strapping pin,
  USB/UART, I2C or a sensor pin.
* B's banner and `stats` print `EEPROM power: GPIO6`, so a log always
  shows which supply a board used.

Why it is enough: the AT24C256 draws about 3 mA at most while writing
and microamps in standby, well within a GPIO's drive. The pin sits
low (unpowered EEPROM) only from reset until `setup()` reaches the
EEPROM, before the store is touched.

Risk to check on the board: until GPIO6 is driven, the module's
pull-ups connect SDA/SCL to an unpowered VCC. GPIO9 is a strapping
pin, and low at reset means download mode, so B must still print its
banner after a power cycle with the EEPROM wired. If it does not,
fall back to running without B.

**Real fix:** resolder B's 3V3 header pin, wire VCC back to 3V3 and
drop the flag from the two node-b envs.
