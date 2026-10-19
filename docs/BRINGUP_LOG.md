# Bring-up log

Fill this in **during** bring-up, not afterwards. It is the evidence for timeline adherence, problem-solving and individual contribution. Plan and step numbers refer to `docs/BRINGUP.md` (schedule §3.5, steps §3.4, sensors §4.6–4.8) and test ids to `docs/HARDWARE_TEST_PLAN.md`.

Conventions:
* **Time:** local 24 h, `Sun 18 10:42`. Durations in hours with one decimal (`0.5`).
* **Who:** initials of whoever did the work. Name both people if you paired: `NS+TM`.
* **Photos:** `docs/photos/<day>-<hhmm>-<step>.jpg`, e.g. `docs/photos/18-1042-step2-eeprom.jpg`. Photograph wiring before you power it, plus anything that went wrong.
* **Logs:** serial logs go to `docs/test-logs/<test>-node-<x>.log` (HARDWARE_TEST_PLAN "Logs"). Put the file name in the row.
* **Status:** `done`, `slipped` (finished late), `cut` (deliberately dropped; say why), `blocked`.

## 1. Milestones: planned vs actual

Pre-filled from BRINGUP §3.5 / §4.7. Fill in the right-hand columns as you go.

| id | milestone | planned slot | planned h | started | finished | actual h | status | evidence (logs, photos) | who |
|---|---|---|---|---|---|---|---|---|---|
| M0 | WSL usbipd + dialout + udev; Pi SD card + UART config (§3.2) | Sat 17 evening | 1.0 | | | | | | |
| M1 | Step 0: read MACs, label boards, compare with `transport.cpp:24-38` | Sun 18 AM | 0.5 | Sun 18 15:49 | Sun 18 15:56 | 0.1 | done | all 4 attached via `usbipd --auto-attach`; by-id MACs match the table, no edit. Labels: 1=A (`C5:D0:18`, port 1-1), 2=D (`C5:C1:9C`, hub 5-2), 3=B (`C5:C3:1C`, hub 5-3), 4=C (`C8:A3:08`, hub 5-4) | |
| M2 | Step 1: one board, no EEPROM (banner, MAC check, FATAL) | Sun 18 AM | 0.5 | Sun 18 15:58 | Sun 18 16:06 | 0.1 | done (late slot) | done on all four, one at a time, normal images (`check_images.sh`: FI compiled out). Every banner: right env, `MAC … matches the table entry`, then FATAL every 5 s. `step1-node-{a,b,c,d}.log`. Problems 1–3 | |
| M3 | Step 2: EEPROM on one board (fresh format, set/get across power cycle) | Sun 18 AM | 1.0 | Sun 18 17:17 | Mon 19 07:03 | 0.5 | slipped (problem 4 overnight) | Mon 19 06:54 node-a flashed on board 1; 06:55 no FATAL, `storage fresh format`, `SET x: UNDER_REPLICATED (served by A …)`, `x = "1"`, storage errors 0, stack headroom 13208 B. 07:01 unplug/replug: `reset reason: power-on`, `storage recovered: 6 live`, `x = "1"`, storage errors 0; **HLC continued**: `hlc 37466.0` at uptime 37 s before, `hlc 466675.0` at uptime 73 s after (RESULTS `hlc_reboot`, pass). `step2-node-a.log`. Actual h excludes the rewiring time | NS |
| M4 | Step 3: two nodes (view `AB--`, `set k20` served by A) | Sun 18 AM | 0.5 | Mon 19 07:47 | Mon 19 08:01 | 0.3 | done with **A + C** (B deferred, problem 6) | C step 2: 07:49 no FATAL, `storage fresh format`, already `view A-C-`, 10 keys pushed by anti-entropy, `y = "2"`; 07:59 power cycle: `power-on`, `storage recovered: 12 live`, 18 page writes since boot (no reformat, so WP is low). Step 3: `view A-C-` on both, `tx -> C: 174 delivered, 0 failed`, `unknown MAC 0`. `k11` (A+C): set on A `OK (served by A)`, get on C `"hello-ac"`. `k6` (C+B) and `k4` (B+C): `ring` showed B's role handed to A in the current view, so `OK (served by C)`, not UNDER_REPLICATED (expected with two live nodes); read back on A. `step2-node-c.log`, `step3-A-C.log` | NS |
| M5 | Step 4: four nodes, pre-flight, **H1**, **H2**, H3 | Sun 18 AM | 1.5 | Mon 19 08:37 | Mon 19 08:50 | 0.3 | partial: **three nodes A + C + D** (B deferred, problem 6); formal H1-H3 not run yet | D step 2: 08:38 no FATAL, `storage fresh format`, `view A-CD`, `z = "3"`; 08:41 power cycle: `power-on`, `storage recovered: 20 live`, 20 page writes (WP low). `view A-CD` on A, C, D, every peer alive. Via A: `k2` (D+C) `OK (served by D)`, `k8` (A+D) `OK (served by A)`, `k9` (B+D, B's role handed to A) stored on D; read back on C and D. **Failure/rejoin of D:** unplugged ~08:45:50; A shows `D DEAD`, view `A-C-`; 7 writes via A to keys D owns or replicates (k2 k3 k24 k18 k19 → `OK (served by C)`, k8 k9 → `OK (served by A)`), all as `ring` predicted, no UNDER_REPLICATED. Replugged 08:48:21: D `ACTIVE`, `storage recovered: 27 live`, `dump` shows all 7 at the versions C/A assigned, `resync applied 19`, `get k2`/`get k9` served by D. 0 failed sends to D from A and C, storage errors 0 on all three. `step2-node-d.log`, `step4-A-C-D.log` | NS |
| M6 | Step 5: Pi bridge (loopback, `ping`, `sensors`), **H14** | Sun 18 PM | 1.5 | | | | | | |
| M7 | Baseline numbers: `scripts/measure.py` latency section, `members` idle | Sun 18 PM | 1.0 | | | | | | |
| M8 | Core fault story: **H6**, **H7**, **H8**, **H11**, **H12a**, H9 | Mon 19 AM | 4.0 | | | | | | |
| M9a | Sensor A: BH1750 + EEPROM coexistence (§4.6) | Mon 19 PM | 1.0 | | | | | | |
| M9b | Sensor B: AM312 + settle-time measurement | Mon 19 PM | 1.5 | | | | | | |
| M9c | Sensor C: LM393 + knob setting | Mon 19 PM | 1.0 | | | | | | |
| M9d | Sensor D: DHT11 + radio check (§4.8, bit off vs on) | Mon 19 PM | 1.5 | | | | | | |
| M9e | All four sensors together, 30 min, logs | Mon 19 PM | 1.0 | | | | | | |
| G1 | **Go/no-go** per sensor at 17:00 (record which bits stay set) | Mon 19 17:00 | — | | | | | | |
| M10 | Overnight soak (H13, shortened), started | Mon 19 night | 0.5 | | | | | | |
| M11 | Read soak; `measure.py` sensor + wear sections | Tue 20 AM | 0.5 | | | | | | |
| M12 | Fault-injection images: H4, H5, H10, `measure.py --loss` (10 %, 20 %) | Tue 20 AM | 1.0 | | | | | | |
| M13 | H12b (power cut during compaction) | Tue 20 AM | 0.5 | | | | | | |
| M14 | RESULTS.md hardware column filled, "why they differ" written | Tue 20 PM | 1.0 | | | | | | |
| M15 | Demo rehearsal 1 (DEMO.md, timed) | Tue 20 PM | 1.0 | | | | | | |
| M16 | Demo rehearsal 2 + slides | Tue 20 PM | 2.0 | | | | | | |
| M17 | **Demo** | Tue 20 | — | | | | | | |
| | **total** (BRINGUP §3.5's 23.5 h + 0.5 h for M14 + 0 for go/no-go) | | **24.0** | | | | | | |

## 2. Problem log

One row per problem, even small ones: a wrong cable counts. Write the symptom as observed (exact message), the cause once found, and the fix. "Time spent" runs from first noticing the problem to having it fixed.

| # | time | step / test | symptom (exact) | cause | fix | time spent (h) | photo / log | who |
|---|---|---|---|---|---|---|---|---|
| e.g. | Sun 18 10:42 | M3 / step 2 | `FATAL: AT24C256 not answering on I2C (SDA 8, SCL 9, addr 0x50)` every 5 s | SDA and SCL swapped on the breadboard | swapped GPIO8/9 wires; checked pull-ups 4.7 kΩ with meter | 0.2 | `18-1042-step2-eeprom.jpg` | NS |
| 1 | Sun 18 16:02 | M2 / step 1 | opening the console (`pio device monitor -e node-a`, and pyserial with DTR/RTS set low) reboots the board: ROM boot text + banner each time | `monitor_rts = 0`/`monitor_dtr = 0` make miniterm drive DTR low then RTS low after Linux asserted both on open; DTR low + RTS high for an instant is the USB-Serial-JTAG reset condition. Opening with both lines left asserted does **not** reset (checked). BRINGUP.md Phase 2 "Monitor reset" row was wrong | dropped the two `monitor_*` lines from `platformio.ini` (comment left in their place); re-tested at 16:15 with `pio device monitor -e node-d`, opened twice: no ROM text, no banner, FATAL loop continues. First open shows a burst of old buffered FATAL lines. BRINGUP.md Phase 2 row corrected | 0.2 | `step1-node-a.log` | |
| 2 | Sun 18 16:01 | M2 / step 1 | `stats` gets no reply on a bare board | `halt()` in `setup()` (`main.cpp:427-429`) loops before the console/node task starts. Expected; but BRINGUP.md:15 ("type `stats` to see the identity lines again") does not hold without EEPROM | banner captured instead by opening the port (which resets, problem 1) | 0.0 | | |
| 3 | Sun 18 16:01 | M2 / step 1 | `reset reason: unknown` after upload and after console open | the reset comes from the USB-Serial-JTAG peripheral; IDF 4.4's `esp_reset_reason_t` has no USB value, so it maps to `ESP_RST_UNKNOWN` | none needed: on these boards `unknown` = USB/host reset. Power-on, PANIC, BROWNOUT, WDT still report by name | 0.0 | | |
| 4 | Sun 18 17:18 | M3 / step 2 | board 1 (A) with EEPROM wired: `FATAL: AT24C256 not answering on I2C (SDA 8, SCL 9, addr 0x50)` every 5 s; boots normally (banner, MAC match) | open. Throwaway I2C diag (scratchpad, not in repo): SDA and SCL both read high with internal pull-down (external pull-ups present) and are not held low; address scan 1-126 at 100 kHz finds **no device** on SDA 8/SCL 9 or swapped (0x50: NACK, Wire error 2). So the bus is idle and pulled up but the chip never ACKs. **Resolved (Mon 19, before 06:50):** most likely a pin-order mismatch or a bad contact in the first direct wiring (module header order differs from the SuperMini's) | rewired on a breadboard following each module's silkscreen. Every module then answers alone at 0x50 and 0x58 (0x58 = the chip's ID/security block). Pull-ups are on the modules, 9.84 kΩ on SDA and SCL (measured), so not a pull-up problem. GY-302 BH1750 also seen at 0x23 on board 1. BRINGUP.md: wire by silkscreen, 0x58 never written; driver guarded at compile time (`eeprom.h`, `config.h`) | | | NS |
| 5 | Mon 19 07:02 | M3 / step 2 | first console command after the replug: `> edge-kv firmwar.795 [A] state -status` → `unknown command 'edge-kv' (try 'help')` | the node **received** pieces of its own boot text as console input. Most likely the new tty starts in cooked mode with ECHO on, and the board's buffered boot output arrives before the reader switches it to raw, so Linux echoes it back. Afterwards the port reads `-echo -icanon` (raw). Not proven which opener raced; ModemManager is inactive | none in code yet. No boot line parses as a harmful command (only `sensor light: …`, which `sensor` ignores). Rule: open ports only with `pio device monitor` or pyserial, **never `cat`/`echo` on the tty**: in cooked mode every reply line is echoed back as a command (`unknown command …` feeding itself). After a replug, send one empty line before the first real command. 08:38 on D the echo produced **two** commands (`edge-kv`, a fragment `se`): fragments can start mid-word, so `reset reason: power-on` could in principle arrive as `set reason: power-on` and write a junk key. Candidate fix (not applied): ignore console input for ~1 s after USB connects | 0.1 | `step2-node-a.log` | |
| 6 | Mon 19 (before 07:49) | M4 / step 3, board 3 (B) | B's EEPROM could not be powered reliably: the SuperMini's 3V3 header pin is loose | loose 3V3 header pin on board 3 (B); no soldering iron on site | **deferred.** Board 3 left as it is (normal image `node-b`, FATAL loop, no EEPROM). Tests continue with A, C, D; B counts as a failed member, so writes to keys B owns report UNDER_REPLICATED or are handed off (correct behaviour, D5). GPIO6-power workaround not started (only on request) | | | NS |
| 7 | | | | | | | | |
| 8 | | | | | | | | |
| 9 | | | | | | | | |
| 10 | | | | | | | | |

## 3. Decisions made on the day

Plan changes and why. Examples: cutting H9, keeping a sensor simulated, retuning a timeout in `config.h`.

| time | decision | reason | alternatives considered | who |
|---|---|---|---|---|
| Sun 18 16:15 | removed `monitor_rts = 0`/`monitor_dtr = 0` from `platformio.ini` | they made every monitor open reboot the node (problem 1), which would wipe RAM state in the middle of fault tests | keep them and treat each open as a reboot; open the port with our own pyserial script | |
| Mon 19 07:13 | reflashed B, C, D (normal images) from `f3b5e1c` before their EEPROMs are wired; banners `built Oct 19 2026 07:13:24/41/56`, MACs match. Node A's image (built 06:54:24, after `b81c370`) has the same firmware source: no change to `src/`, `include/` or `platformio.ini` since `b81c370` | every hardware log from step 3 on comes from one firmware source | leave B-D on the Oct 17 images (same runtime behaviour, only the address checks differ) | NS |

## 4. End-of-day summary

| day | planned h | actual h | milestones done | slipped | cut | biggest problem | next morning starts with |
|---|---|---|---|---|---|---|---|
| Sun 18 | 6.5 | | | | | | |
| Mon 19 | 10.5 | | | | | | |
| Tue 20 | 6.0 | | | | | | |

## 5. Individual contributions

Fill in at the end from the "who" columns above, with links to the rows and commits.

| person | bring-up / wiring | firmware / code | tests run (ids) | problems solved (log #) | docs / demo |
|---|---|---|---|---|---|
| | | | | | |
| | | | | | |
