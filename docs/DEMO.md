# Demo script (about 5 minutes of commands)

Four ESP32-C3 nodes (A–D) form a leaderless key-value store over ESP-NOW. The whole demo runs on the four node consoles on the laptop. If the hardware misbehaves, the same script runs in the simulator ("Fallback" at the end).

Expected outputs come from the **hardware rehearsal of Sun 25 Oct** (`docs/test-logs/sun25-demo-rehearsal.log`) unless marked otherwise. Numbers (latencies, hlc, sequence numbers, key counts, sensor values) will differ on the day. Key placement does not: it depends only on the key name.

**Sensors:** A light (BH1750), C sound (LM393) and D temperature (DHT11) are real. **B's motion is simulated** (its AM312 gave no output, BRINGUP_LOG problem 24), and every B reading says `simulated`. C's LM393 only hears a loud clap within ~5 cm of its microphone, and even then not every time.

## Setup checklist (before the audience arrives, not counted)

1. **Firmware: the `-fi` images from tag `v1.0-presentation`** (`git 2417e28`, frozen). Step f needs them; they behave exactly like the normal images until a fault is armed. On each console, `stats` must print `env node-x-fi, git 2417e28`. If any board shows something else, flash from the tag:
   ```bash
   git checkout v1.0-presentation
   make firmware                      # all 8 images, then scripts/check_images.sh: all "ok  git 2417e28"
   .venv/bin/pio run -e node-a-fi -t upload --upload-port /dev/serial/by-id/<board A>   # same for B–D
   git checkout hw-bringup
   ```
2. **Placement:** boards at least 20 cm apart, antenna end (opposite the USB-C) clear of wires, breadboards and the laptop (BRINGUP_LOG problem 14: close together, views flap). Wire by silkscreen.
3. **USB into WSL:** in an admin PowerShell, `usbipd list`, then `usbipd attach --wsl --busid <BUSID> --auto-attach` for each board (last seen: A 1-1, B 5-4, C 5-3, D 5-2; check `usbipd list`, they change when a cable moves).
4. **Consoles:** one per board, by-id paths only, tiled A B / C D:
   `.venv/bin/pio device monitor -p /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_14:63:93:<MAC>-if00 -b 115200`
   (MAC suffix A `C5:D0:18`, B `C5:C3:1C`, C `C8:A3:08`, D `C5:C1:9C`). **After opening a console or after a replug, press Enter once (empty line) and wait ~1.2 s** before typing a command (boot input hold, BRINGUP_LOG problems 9/10).
5. **Health:** `stats` on each console: `storage errors 0`, `aborted 0`.
6. **Demo settings:** on every console `sensor period 5000`, so readings change visibly. On B and D `log off` to keep their consoles quiet; A and C stay at `log info`, their log lines are part of the show.
7. A console reply seems missing? Press Enter: the next input brings out anything still waiting on the board.

## a. The cluster is up (30 s)

| console | command |
|---|---|
| A, B, C, D | `status` |
| A | `members` |

**Audience sees** (A's `status`; the other three are the same shape, with `sensor motion (simulated)` on B, `sensor sound (hardware)` on C, `sensor temp (hardware)` on D):

```
node A  state ACTIVE  view ABCD  uptime 129s
storage recovered: 21 live, 4 tombstones, 25/64 keys, log 10938/16064 bytes, bank B, 0 compactions
sensor light (hardware): on, 5 samples, last 10.833 lux seq 4210, 8 ok, 2 under-replicated, 0 failed, 0 read failures
> members
  A  self    ACTIVE
  B  alive   last heard 102 ms ago, 217 heartbeats
  C  alive   last heard 399 ms ago, 213 heartbeats
  D  alive   last heard 288 ms ago, 217 heartbeats
```

**Idea:** four nodes, every one sees view `ABCD`, and there is no leader: each node broadcasts a heartbeat every 500 ms and keeps its own view of who is alive.

## b. Live sensor readings (1 min 15 s)

| console | command / action |
|---|---|
| B | `sensors`, then again every ~5 s (6-8 times) |
| table | cover A's light sensor with a hand (~10 s), or shine a phone torch on it |
| table | breathe on D's DHT11 from close up for **~20 s** (10 s gave only +0.5 °C) |
| table | clap loudly **within ~5 cm of C's microphone** |

**Audience sees** (every `sensors` prints the four latest readings and which node served each):

```
s/C/sound = 0 events, 0.0 % active  (seq 3103, sampled at hlc 80233223)
   (served by B, 1 attempt(s), 0 ms)
s/D/temp = 29.300 degC  (seq 4489, sampled at hlc 80236319)
   (served by B, 1 attempt(s), 0 ms)
s/A/light = 0.000 lux  (seq 4222, sampled at hlc 80233602)
   (served by C, 1 attempt(s), 7 ms)
s/B/motion = 3 events, last motion 4 s before the sample  (seq 2702, sampled at hlc 80232521, simulated)
   (served by C, 1 attempt(s), 9 ms)
```

* **Light:** covered, `0.000 lux`; uncovered, back to room light (24 lux in the rehearsal). A torch drives it into the thousands (3168 lux, BRINGUP_LOG M9a).
* **Temperature:** 29.3 → 29.8 °C over ~20 s of breathing in the rehearsal. The DHT11 is slow; say so before starting.
* **Sound:** in the rehearsal two sharp claps at 5 cm gave **0 events**. If nothing registers, say so: the module is the limit, not the store.
* **B:** its line ends in `simulated`. Point at it: the flag exists so simulated data is never passed off as real.

**Idea:** each sample is an ordinary replicated write into the store. `sensors` reads each one back from wherever it lives: B's own motion reading comes from C.

## c. Write on one node, read on another (30 s)

| console | command |
|---|---|
| B | `set demo/greeting hello-from-B` |
| D | `get demo/greeting` |
| A | `ring demo/greeting` |

**Audience sees:**

```
SET demo/greeting: OK (served by C, 1 attempt(s), 36 ms)
demo/greeting = "hello-from-B"
   (served by C, 1 attempt(s), 6 ms)
key 'demo/greeting' hash c52e626c
  all nodes up : primary C, replica B
  current view : primary C, replica B
```

**Idea:** any node accepts any request and forwards it to the key's owners, which every node computes from the key's hash (consistent hashing, no lookup table); the primary copies each write to a replica before answering.

## d. Unplug node C (45 s)

Never unplug A: its console and light sensor stay up for the rest of the demo.

| console | command / action |
|---|---|
| table | unplug **C**'s USB cable |
| A | watch the log (~2 s), then `members` |
| B | `sensors` |
| B | `set k6 written-while-C-down` |
| A | `ring k6` |
| D | `get k6` |

**Audience sees** (`view: AB-D` 1.8 s after the unplug):

```
  [A] view: AB-D
> members
  C  DEAD    last heard 2713 ms ago, 658 heartbeats
s/C/sound = 0 events, 0.0 % active  (seq 3133, ...)
   (served by B, 1 attempt(s), 0 ms)
s/B/motion = 2 events, last motion 4 s before the sample  (seq 2733, ..., simulated)
   (served by D, 1 attempt(s), 7 ms)
s/A/light = 23.333 lux  (seq 4252, ...)
   (served by A, 1 attempt(s), 7 ms)
SET k6: OK (served by B, 1 attempt(s), 26 ms)
key 'k6' hash 993d8254
  all nodes up : primary C, replica B
  current view : primary B, replica A
k6 = "written-while-C-down"
   (served by B, 1 attempt(s), 6 ms)
```

**Idea:** a node is declared dead after 2 s without heartbeats; only its keys move, each to the next node on the ring, so writes keep succeeding and every reading is still served, including C's own last sound reading from its replica.

## e. Plug C back in: catch-up and repair (45 s)

| console | command / action |
|---|---|
| table | plug **C** back in (without leaning over the other boards) |
| C | when the console reattaches (~6 s): Enter, wait ~1.2 s, then `dump` |

**Audience sees:** A logs `view: ABCD`; C's console catches its last boot line `state -> ACTIVE` (7.2 s after power-on); `dump` on C holds the key written while it was away:

```
  [C] state -> ACTIVE
  [A] view: ABCD
> dump
  ...
  v=80304159.1@C  demo/greeting = "hello-from-B"
  v=80389123.1@B  k6 = "written-while-C-down"
  ...
12 key(s); log 1911/16064 bytes used
```

The console reattaches too late to show the earlier boot lines; the buffered banner can come back as `unknown command 'edge-kv'` (harmless, BRINGUP_LOG problem 10). In the rehearsal A's view flapped for ~30 s after the replug (heartbeat loss, problem 14); it settles at `ABCD` by itself. What C logs unseen (from the simulator): `state -> BOOTING`, `state -> CATCHUP`, `anti-entropy: key k6 repaired from A`, `state -> ACTIVE`.

**Idea:** a returning node stays silent (BOOTING), announces what it has (CATCHUP) while the others push everything newer, and only then serves (ACTIVE), so it never answers with stale data. The same anti-entropy exchange runs every 10 s to repair any missed replica.

## f. 20 % packet loss (1 min)

| console | command |
|---|---|
| A, B, C, D | `fault loss 20` |
| A | `bench 50` (~30 s) |
| A, B, C, D | `fault clear` |

**Audience sees:**

```
frame loss 20 % on every frame this node sends
bench: 50 sequential SETs on keys bench/0..7
  [C] SET_REQ bench/1 from A#2586506400: duplicate, cached response replayed
  [C] REPL_REQ D#4084169801 duplicate: cached REPL_ACK replayed
  ...
bench: 50 ok, 0 failed in 30400 ms; latency min 26 avg 608 max 4904 ms
faults cleared
```

The `duplicate … replayed` lines (14 in the rehearsal) appear on the consoles of the nodes that are primaries for the bench keys, mostly C. A `replica D did not answer for bench/6: under-replicated` line can appear too; anti-entropy repairs that copy within ~10 s. `fault clear` answers `faults cleared` on the frozen images (the rehearsal ran on an earlier image that answered `fault armed`).

**Idea:** a lost packet is retransmitted with the same request id, and the receiver's dedup table replays the cached answer, so every write still executes exactly once: latency goes up, failures stay at 0.

## After the demo (not counted)

Delete the demo keys, so they do not take slots in the 64-key per-node index (BRINGUP_LOG problem 13). On any console:

```
del bench/0   (… through bench/7)
del demo/greeting
del k6
```

Each answers `DEL <key>: OK`. Then `sensor period 30000` on every console, `log info` on B and D, and `stats` on every console: `storage errors 0`, `aborted 0`.

## Rehearsal timings (hardware, Sun 25 Oct)

Command time only, scripted: a 4 s, b 60 s, c 3 s + 3 s `ring`, d 10 s after the unplug (view `AB-D` 1.8 s after it), e 27 s from replug to `dump`, f 34 s. Typed by hand with pauses for the audience, the budget above is about 5 minutes; the rest of the slot is narration.

## Closing line (15 s)

Leaderless placement by consistent hashing, RF = 2 replication, hybrid logical clocks for versions, retries with deduplication, and anti-entropy repair. These are the ideas behind Dynamo and Cassandra, running on four low-cost microcontrollers with a 32 KB EEPROM each.

## Backup video

Backup video of a full hardware run: **not recorded yet** (no file in the repo). NS: record it at the last rehearsal and put its location here. Play it only if both the boards and the simulator fail.

---

## Fallback: the same demo in the simulator

Use this if the boards or the radio misbehave on the day. Everything runs on the laptop, in one terminal. Build it beforehand: `make sim`, then `build/host/edge_sim`. Type node commands as `<node>: <command>`. All readings are flagged `simulated`, so say so; the physical stimuli in step b do nothing. Key placement and the outputs above are the same, because it is the same firmware logic compiled for the PC.

| step | simulator commands |
|---|---|
| setup | `A: sensor period 5000` … `D: sensor period 5000`, `B: log off`, `D: log off` |
| a | `A: status`, `B: status`, `C: status`, `D: status`, `A: members` |
| b | `B: sensors` (a few times, ~5 s apart) |
| c | `B: set demo/greeting hello-from-B`, `D: get demo/greeting`, `A: ring demo/greeting` |
| d | `off C`, then `A: members`, `B: sensors`, `B: set k6 written-while-C-down`, `A: ring k6`, `D: get k6` |
| e | `on C`, wait ~6 s, `C: dump` |
| f | `A: fault loss 20` … `D: fault loss 20`, `A: bench 50`, `A: fault clear` … `D: fault clear` (or `loss 0.2` / `loss 0`) |
| cleanup | `C: del bench/0` … `C: del bench/7`, `C: del demo/greeting`, `C: del k6` |

What differs from the boards (simulator run Wed 28 Oct, seed 1):

* `status` has no `storage recovered` or `chip temp` line, and every sensor says `(simulated)`; `stats` has no `storage errors` line.
* Step e is visible live: `storage recovered: 6 live`, `state -> BOOTING`, `state -> CATCHUP`, `anti-entropy: key k6 repaired from A`, `state -> ACTIVE` 5.5 s after `on C`; `dump` ends `7 key(s); log 1489/16064 bytes used`.
* Step f: `bench: 50 ok, 0 failed`, but slower than the boards and different on every run even with the same seed (the simulator is paced by the wall clock): two runs gave 34617 ms / avg 692 / max 3712 ms and 43113 ms / avg 862 / max 4314 ms. Allow ~45 s. `faults cleared` on all four.
