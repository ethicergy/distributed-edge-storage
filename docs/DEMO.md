# Demo script (10 minutes)

Four ESP32-C3 nodes (A–D) form a leaderless key-value store over ESP-NOW. Node A bridges to a Raspberry Pi. The demo uses only node consoles and the Pi CLI. If the hardware misbehaves, the same script runs in the simulator ("Fallback" at the end).

The expected outputs below come from a full rehearsal in `build/host/edge_sim` on 2026-10-17. Numbers (latencies, hlc, sequence numbers, sensor values) will differ. Key placement does not: it depends only on the key name.

## Before the audience arrives (not counted)

1. Place the boards: boards at least ~20 cm apart, antenna end (the end opposite the USB-C) clear of wires, breadboards and the laptop (BRINGUP_LOG problem 14: close together, views flap). Power the four boards. Open one console per board on the laptop (`pio device monitor -p /dev/serial/by-id/<board> -b 115200`), tiled A B / C D.
2. SSH into the Pi in one terminal: `cd ~/distributed-edge-storage/pi`.
3. On every console: `sensor period 5000`, so readings change visibly. On B and D, `log off` to keep their consoles quiet. Keep A and C at the default `log info`: their log lines are part of the show.
4. Check that the Pi's `python3 -m edgekv ping` prints `bridge is node A`.
5. Doing step 7 on the boards? Flash the `-fi` images beforehand (`pio run -e node-a-fi -t upload …`, same for B–D). Otherwise step 7 uses the simulator.
6. If a console reply seems missing, press Enter: the next input always brings out anything still waiting on the board (BRINGUP_LOG problem 9, fixed in the firmware since Tue 20).
7. Run only one Pi command at a time. A second command while `watch` runs is refused with ``error: port busy: another edgekv command is using it, stop `watch` first``. Stop `watch` with Ctrl-C first.

## 1. The cluster is up (1 min)

| where | command |
|---|---|
| each console | `status` |
| console A | `members` |

**Audience sees:**

```
node A  state ACTIVE  view ABCD  uptime 65s
storage recovered: 6 live, 0 tombstones, 6/64 keys, log 455/16064 bytes, ...
sensor light (simulated): on, 3 samples, last 389.097 lux seq 2, 6 ok, ...   <- "hardware" once A's bit is set
> members
  A  self    ACTIVE
  B  alive   last heard 45 ms ago, 31 heartbeats
  C  alive   last heard 45 ms ago, 31 heartbeats
  D  alive   last heard 43 ms ago, 31 heartbeats
```

**Idea:** there is no leader and no coordinator. Every node broadcasts a heartbeat every 500 ms and keeps its own view of who is alive.

## 2. Live sensor readings on the Pi (1.5 min)

| where | command |
|---|---|
| Pi | `python3 -m edgekv watch --interval 5 --csv demo.csv` |
| room | wave at B's PIR, clap near C, cover A's light sensor, breathe on D's DHT11 |
| Pi | Ctrl-C, then `head -5 demo.csv` |

**Audience sees** (every 5 s):

```
A  light        394.028 lux   seq 3      at 12:36:40
B  motion             3 events  last motion 4 s ago seq 2      at 12:36:36
C  sound              3 events  11.0 % active seq 2      at 12:36:37
D  temp          23.299 degC  seq 0      at 12:36:47
```

and a CSV with `read_at,node,kind,value,unit,…,last_motion_s,active_pct,status`. If a module isn't enabled, its line ends in `simulated`. Say so; the flag exists so simulated data is never passed off as real.

**Idea:** each sample is an ordinary replicated write into the store. The Pi reads it back through node A from wherever it is stored. Times are converted from the cluster's hybrid logical clock.

## 3. Write on one node, read on another (1.5 min)

| where | command |
|---|---|
| console B | `set demo/greeting hello-from-B` |
| console D | `get demo/greeting` |
| Pi | `python3 -m edgekv set demo/pi hi-from-the-pi` |
| console C | `get demo/pi` |

**Audience sees:**

```
SET demo/greeting: OK (served by C, 1 attempt(s), 11 ms)
demo/greeting = "hello-from-B"
   (served by C, 1 attempt(s), 8 ms)
OK
demo/pi = "hi-from-the-pi"
   (served by A, 1 attempt(s), 5 ms)
```

**Idea:** any node accepts any request and forwards it to the key's primary. The primary versions the write and copies it to a replica (replication factor 2) before answering.

## 4. Where does a key live? (1 min)

| where | command |
|---|---|
| console A | `ring demo/greeting`, `ring s/B/motion`, `ring k6` |

**Audience sees:**

```
key 'demo/greeting' hash c52e626c
  all nodes up : primary C, replica B
  current view : primary C, replica B
key 's/B/motion' hash a8053102
  all nodes up : primary C, replica D
key 'k6' hash 993d8254
  all nodes up : primary C, replica B
```

**Idea:** consistent hashing. Every node computes the same owners from the key's hash, with no lookup table. B's motion readings are stored on C and D, not on B.

## 5. Pull the plug on C (2 min)

| where | command |
|---|---|
| table | unplug node C's USB cable |
| console A (within ~2 s) | watch the log, then `members` |
| Pi | `python3 -m edgekv sensors` |
| console B | `set k6 written-while-C-down` |
| console A | `ring k6` |
| console D | `get k6` |

**Audience sees:**

```
  [A] view: AB-D
> members
  C  DEAD    last heard 3668 ms ago, 43 heartbeats
```

The Pi still lists all four readings. B's motion comes from its replica D. C's own last sound reading is still served too, because it is stored on B and D:

```
B  motion             3 events  last motion 8 s ago seq 4      at 12:36:46
C  sound              4 events  11.7 % active seq 3      at 12:36:42
```

```
SET k6: OK (served by B, 1 attempt(s), 5 ms)
key 'k6' hash 993d8254
  all nodes up : primary C, replica B
  current view : primary B, replica A
k6 = "written-while-C-down"
   (served by B, 1 attempt(s), 5 ms)
```

**Idea:** failure detection by heartbeat timeout (2 s). Only C's keys move, each to the next node on the ring, and the cluster keeps accepting writes. A modulo-N placement would have moved most keys.

## 6. Plug C back in: catch-up and repair (1.5 min)

| where | command |
|---|---|
| table | plug node C back in |
| console C | watch the boot log, then `dump` |

**Audience sees** on C, within about 6 s:

```
  [C] storage recovered: 7 live, 0 tombstones, ...
  [C] state -> BOOTING
  [C] state -> CATCHUP
  [C] anti-entropy: key k6 repaired from A
  [C] anti-entropy: key s/B/motion repaired from D
  [C] state -> ACTIVE
> dump
  v=33048.1@B  k6 = "written-while-C-down"
  v=23440.1@C  demo/greeting = "hello-from-B"
  ...
```

**Idea:** a returning node stays silent (BOOTING), announces what it has (CATCHUP) while the others push everything newer, and only then serves (ACTIVE). It never answers with stale data. The same anti-entropy exchange runs every 10 s to repair any missed replica.

## 7. Optional: 20 % packet loss (1 min)

This needs the fault-injection images (`node-a-fi` … `node-d-fi`) on all four boards; see "Before the audience arrives". They behave exactly like the normal images until a fault is armed. Their banner says `[fault injection build]`.

| where | command |
|---|---|
| each console (A, B, C, D) | `fault loss 20` |
| console A | `bench 50` |
| each console | `fault clear` |

**Audience sees** (on each console, then on A):

```
frame loss 20 % on every frame this node sends
bench: 50 sequential SETs on keys bench/0..7
  [D] SET_REQ bench/3 from A#62641: duplicate, cached response replayed
  [A] REPL_REQ D#86360 duplicate: cached REPL_ACK replayed
  ...
bench: 50 ok, 0 failed in 36594 ms; latency min 7 avg 731 max 5211 ms
```

The `duplicate … replayed` lines appear on the consoles of the nodes that are primaries for the bench keys. Simulator reference numbers: avg 731 ms, max 5.2 s, 0 failed (`docs/RESULTS.md` §3, `loss20_A`).

**Fallback (normal images on the boards, or no time to reflash):** run it in the simulator on the laptop (`build/host/edge_sim`). Same mechanism, same output:

```
A: fault loss 20
B: fault loss 20
C: fault loss 20
D: fault loss 20
A: bench 50
A: fault clear
B: fault clear
C: fault clear
D: fault clear
```

(`loss 0.2` / `loss 0` in the simulator does the same from the simulated radio's side, in one line.) If you type `fault loss` on a normal image, the console answers `fault injection not compiled in`: the normal images cannot drop packets on purpose (`scripts/check_images.sh`, run by `make firmware`).

**Idea:** lost packets are retransmitted with the same request id, and the receiver's dedup table replays the cached answer. Every write still executes exactly once (all 50 ok, latency goes up, no failures).

## Closing line (15 s)

Leaderless placement by consistent hashing, RF = 2 replication, hybrid logical clocks for versions, retries with deduplication, and anti-entropy repair. These are the ideas behind Dynamo and Cassandra, running on four low-cost microcontrollers with a 32 KB EEPROM each.

---

## Fallback: the same demo in the simulator

Use this if the boards or the radio misbehave on the day. Everything runs on the laptop. Build it beforehand: `make sim`.

```bash
build/host/edge_sim            # prints: Pi bridge (node A) on pseudo-terminal: /dev/pts/N
```

Type node commands as `<node>: <command>` in the simulator window; the other client runs in a second terminal:

| step | hardware | simulator |
|---|---|---|
| setup | `sensor period 5000` on each console | `A: sensor period 5000` … `D: sensor period 5000` |
| 1 | `status`, `members` | `A: status`, `B: status`, …, `A: members` |
| 2 | Pi: `python3 -m edgekv watch …` | second terminal: `cd pi && python3 -m edgekv --port /dev/pts/N watch --interval 5 --csv demo.csv` (Ctrl-C after) |
| 3 | `set` / `get` on consoles, Pi `set` | `B: set demo/greeting hello-from-B`, `D: get demo/greeting`, `python3 -m edgekv --port /dev/pts/N set demo/pi hi-from-the-pi`, `C: get demo/pi` |
| 4 | `ring …` | `A: ring demo/greeting`, `A: ring s/B/motion`, `A: ring k6` |
| 5 | unplug C | `off C`, then `A: members`, Pi `sensors`, `B: set k6 written-while-C-down`, `A: ring k6`, `D: get k6` |
| 6 | plug C in | `on C`, wait ~6 s, `C: dump` |
| 7 | `-fi` images: `fault loss 20` on all four, `bench 50` on A, `fault clear` | `A: fault loss 20` … `D: fault loss 20`, `A: bench 50`, `A: fault clear` … `D: fault clear` (or `loss 0.2` / `loss 0`) |

All readings in the simulator are flagged `simulated`, so say so. Key placement and the outputs above are identical, because it is the same firmware logic compiled for the PC.

If only the Pi link fails on the day, keep the boards. Run step 2 with `sensors` typed on any console instead of the Pi, and skip the Pi `set` in step 3.
