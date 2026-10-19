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
| M3 | Step 2: EEPROM on one board (fresh format, set/get across power cycle) | Sun 18 AM | 1.0 | Sun 18 17:17 | | | | | |
| M4 | Step 3: two nodes (view `AB--`, `set k20` served by A) | Sun 18 AM | 0.5 | | | | | | |
| M5 | Step 4: four nodes, pre-flight, **H1**, **H2**, H3 | Sun 18 AM | 1.5 | | | | | | |
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
| 5 | | | | | | | | |
| 6 | | | | | | | | |
| 7 | | | | | | | | |
| 8 | | | | | | | | |
| 9 | | | | | | | | |
| 10 | | | | | | | | |

## 3. Decisions made on the day

Plan changes and why. Examples: cutting H9, keeping a sensor simulated, retuning a timeout in `config.h`.

| time | decision | reason | alternatives considered | who |
|---|---|---|---|---|
| Sun 18 16:15 | removed `monitor_rts = 0`/`monitor_dtr = 0` from `platformio.ini` | they made every monitor open reboot the node (problem 1), which would wipe RAM state in the middle of fault tests | keep them and treat each open as a reboot; open the port with our own pyserial script | |
| | | | | |

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
