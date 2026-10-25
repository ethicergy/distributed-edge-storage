# Hardware test plan (H1–H14)

Every scenario below already passes in the host simulation
(`make check`, `build/host/test/cluster_test`). This plan repeats them on
the four real boards. A test counts as passed only with a saved serial
log under `docs/test-logs/` (see "Logs").

## 0. Setup

### Wiring

| board | connection |
|---|---|
| every node | AT24C256: SDA → GPIO8, SCL → GPIO9, VCC 3V3, GND, A0–A2 → GND (address 0x50), WP → GND, 4.7 kΩ pull-ups if the module has none |
| node A (bridge) | Pi GPIO14 / TXD (pin 8) → ESP32 GPIO20 (RX); Pi GPIO15 / RXD (pin 10) ← ESP32 GPIO21 (TX); GND ↔ GND. Both 3.3 V logic, no level shifter |

The MAC addresses of the four boards are hard-coded in
`src/platform/transport.cpp`; each board must be flashed with the env
that matches its MAC.

> GPIO20/21 carry the ROM bootloader's messages at reset. The Pi sees a
> burst of text on every reset of node A; the frame decoder skips it.
> If your board routes GPIO20/21 to a USB-UART chip, move the bridge to
> two free GPIOs in `include/config.h` (`BRIDGE_UART_RX_PIN/TX_PIN`).

### Flash

```bash
pio device list                                   # find each board's port
pio run -e node-a -t upload --upload-port /dev/ttyACM0
pio run -e node-b -t upload --upload-port /dev/ttyACM1
pio run -e node-c -t upload --upload-port /dev/ttyACM2
pio run -e node-d -t upload --upload-port /dev/ttyACM3
```

For H4, H5 and H10 flash the fault-injection images instead
(`node-a-fi` … `node-d-fi`); they add the `fault` and `stale` console
commands. Everything else in this plan works with either image.

### Consoles and logs

One terminal per board:

```bash
pio device monitor -p /dev/ttyACM0 -b 115200 -f time -f log2file
```

`log2file` writes `platformio-device-monitor-*.log` in the current
directory. Rename it to `docs/test-logs/<test>-node-<x>.log` after the
test. Type `help` for commands; `log debug` shows every request.

### Pre-flight (do before every session)

On each board:

```
> status
node A  state ACTIVE  view ABCD ...
> members
```

All four boards ACTIVE with view `ABCD`. A board stuck in BOOTING for
more than ~6 s, or a view missing a node, means a radio or MAC problem:
fix that first.

### Keys with known owners

Placement is the same on every board (`ring <key>` shows it). With all
four nodes up:

| primary | replica | key |   | primary | replica | key |
|---|---|---|---|---|---|---|
| A | B | `k20` | | C | A | `k0` |
| A | C | `k11` | | C | B | `k6` |
| A | D | `k8` | | C | D | `k19` |
| B | A | `k14` | | D | A | `k69` |
| B | C | `k4` | | D | B | `k18` |
| B | D | `k9` | | D | C | `k2` |

Sensor keys (primary, replica): `s/A/light` (C, A), `s/B/motion` (C, D),
`s/C/sound` (B, D), `s/D/temp` (B, D).

---

## H1 — every pair reachable

On each board: `members`.

**Pass:** every other node `alive`, last heard < 600 ms, heartbeat
counters increasing; `stats` shows `esp-now rx ... queue overflows 0`
and, per peer, `failed` small relative to `delivered`. Save all four
logs (`H1-node-*.log`).

## H2 — write / read / delete from every node

On node B: `set k20 hello-from-B`, `get k20`, `del k20`, `get k20`.
Repeat from C and D with `k4`, `k0`, `k2` so each node writes a key
owned by every other node.

**Pass:** `SET k20: OK (served by A ...)`, `k20 = "hello-from-B"`,
`DEL k20: OK`, `GET k20: NOT_FOUND`. After the SET, `dump` on the
primary **and** on the replica shows the key at the same version
`v=<ms>.<n>@<primary>`; after the DEL both show `TOMBSTONE`.

## H3 — local fast path

On node A: `set k20 local`, `get k20`, then `stats`.

**Pass:** `served by A`, latency a few ms, and `client ... local`
counter increased by 2 while `client ... attempts` increased by 2 with
no radio request (`radio tx` grows only by heartbeats + the REPL_REQ).

## H4 — retries execute exactly once (fault-injection images)

a) Lost request: on C `fault drop-req 1`, then `set k20 v1`.
   C logs `FAULT: dropped first transmission of SET_REQ`; the SET still
   ends `OK` with `1 attempt(s)` (the retransmission carried it).
b) Lost SET_ACK: on A (primary of `k20`) `fault drop-ack 1`; on C
   `set k20 v2`. A logs `FAULT: dropped SET_ACK ...` then
   `SET_REQ k20 from C#...: duplicate, cached response replayed`.
c) Lost REPL_ACK: on B (replica) `fault drop-repl-ack 1`; on C
   `set k20 v3`. B logs `FAULT: dropped REPL_ACK` then
   `REPL_REQ ... duplicate: cached REPL_ACK replayed`.

**Pass:** each SET answers `OK`; in `stats`, `server executed` on A
grows by exactly 1 per SET and `as replica: applied` on B by exactly 1
per SET; `dump` on A and B shows the same version.

## H5 — duplicate request

On C `fault dup 1`, then `set k20 dup`.

**Pass:** A executes once (`executed` +1) and its `dedup replays` or
`suppressed` grows by 1.

## H6 — replica failure during a write

Unplug B (replica of `k20`). Within 2 s, on C: `set k20 during`.

**Pass:** answer `UNDER_REPLICATED` after about 1.5 s with
`1 attempt(s)`; A logs `replica B did not answer for k20:
under-replicated`. After ~2 s A's log shows `view: A-CD`; `set k20
after` now answers `OK` (a new replica took over: `ring k20`). Plug B
back in: B logs `state -> CATCHUP`, then `anti-entropy: key k20
repaired from ...`; `dump` on B shows `k20 = "after"`.

## H7 — primary failure

`set k20 before-failover` on C. Unplug A (primary of `k20`). Wait 3 s.
On C: `get k20`, `set k20 after-failover`.

**Pass:** `served by B` (the old replica) with the right value; the SET
is served by B; `ring k20` on C shows primary B.

## H8 — recovery without stale reads

Continue from H7 (A holds `before-failover`). On C enable `log debug`
and keep running `get k20` (or `bench`-style repetition by hand) while
plugging A back in.

**Pass:** A logs `state -> BOOTING`, `CATCHUP`, a repair of `k20`, then
`ACTIVE`; no GET ever prints `before-failover`; after A is ACTIVE,
`get k20` is `served by A` with `after-failover`.

## H9 — delete while a node is down

`set k20 zombie`. Unplug B (replica). `del k20` on C (`OK` or
`UNDER_REPLICATED`). Plug B back in, wait 30 s.

**Pass:** `get k20` is `NOT_FOUND` from every node; `dump` on B shows
`k20 TOMBSTONE`, never the old value, through several sync rounds
(`sync` to force one).

## H10 — stale version rejected (fault-injection images)

`set k0 current` (primary C). On D: `stale k0 C`.

**Pass:** C's `stats` shows `resync ... stale` +1; `get k0` still
returns `current`.

## H11 — anti-entropy repairs a replica

Write ten keys: `set k0 v0` … `set k20 v10` (any mix). On B:
`format yes` (B stops serving and catches up).

**Pass:** within ~30 s B logs a series of `anti-entropy: key ...
repaired from ...` and `dump` on B lists every key B owns (check with
`ring`) at the same versions as the other owner.

## H12 — power-cycle persistence, also during compaction

a) Write a few keys, unplug all four boards, plug them back in.
   **Pass:** all keys readable with their last values; `status` shows
   `storage recovered`.
b) On D: `compact`, and cut D's power within a second (the log shows
   `compaction started`). Plug it back in.
   **Pass:** D boots (`storage recovered` or `recovered (torn tail
   discarded)`), `dump` lists the same keys and versions as before.

## H13 — 2-hour sensor soak

Leave all four boards running for 2 hours (default 30 s sample period;
`sensor period 10000` on each node for a harsher run). Optionally power
cycle one node every 20 minutes.

**Pass:** at the end, on every node `status` shows the sensor with
`0 failed` (writes `under-replicated` are allowed around power cycles);
`stats` shows `storage errors 0`, several compactions, `esp-now queue
overflows 0`; `sensors` from any node returns four readings with
sequence numbers ≈ elapsed time / period.

## H14 — Pi bridge, with a node down

On the Pi (`pi/README.md` for setup):

```bash
python3 -m edgekv --port /dev/serial0 ping
python3 -m edgekv --port /dev/serial0 set pi-key hello
python3 -m edgekv --port /dev/serial0 get pi-key
python3 -m edgekv --port /dev/serial0 sensors
```

Unplug C, wait 3 s, repeat `sensors`, `set`, `get`, `del pi-key`.

**Pass:** `bridge is node A`; all commands succeed with C down; C's
sound reading is still returned (it lives on B and D). Save the
Pi terminal output as `docs/test-logs/H14-pi.log`.

---

## Tuning from measurements (P2)

Record on the real boards and adjust `include/config.h` if needed (the
`static_assert`s keep the timeouts consistent):

* `bench 200` on a node: SET latency min/avg/max. Typical avg should be
  well below `CLIENT_INITIAL_TIMEOUT_MS` (600 ms); if not, raise it.
* `members` while idle: `last heard` stays below ~600 ms. If views flap
  (`view:` lines in the log without anyone being unplugged), raise
  `MEMBERSHIP_TIMEOUT_MS` (keep ≥ 3 heartbeats).
* `stats` → `esp-now tx ... failed`: sustained failures to one peer
  indicate range/antenna problems (TX power is 8.5 dBm in
  `transport.cpp`).
