# Hardware bring-up (4× ESP32-C3 SuperMini, AT24C256, Raspberry Pi)

Written 2026-10-17, the day before the hardware arrives. Plan: Day 1 = Sun 18 Oct,
Day 2 = Mon 19 Oct, Day 3 = Tue 20 Oct (demo). Line numbers refer to the tree
with the boot-banner change (see "Changes already made").

## Changes already made (approved)

| change | where | what it does |
|---|---|---|
| A. Boot identity | `src/platform/main.cpp:133-243`, called at `main.cpp:410` and from `stats` (`main.cpp:254`); `transport::configured_mac()` in `include/transport.h:74-75` and `src/platform/transport.cpp:188-191`; `EDGE_ENV_NAME` set by `platformio.ini:29-30` | Prints the env name, build time, `esp_reset_reason()` as text, and the board's STA MAC. Says whether the MAC matches the table entry for `NODE_ID`. On a mismatch it prints a `MAC MISMATCH` block and, if the MAC belongs to another node, which env to flash instead. |
| B. MAC reader | `scripts/read_macs.sh` | Runs `esptool.py read_mac` on each `/dev/ttyACM*` (or the ports you list) and prints `MAC_A`…`MAC_D` blocks in the format of `transport.cpp:24-38`. |

* The build time is the compile time of `main.cpp`. PlatformIO rebuilds that file only when it changes, so after edits to other files the time can be older than the image.
* The banner prints about 1.5 s after reset (`main.cpp:401`). Under WSL the port usually re-attaches later than that, so you may miss it. Type `stats` to see the identity lines again.
* The MAC table itself is unchanged.

---

## Phase 2: hardware assumptions in the code

| area | assumption | source | SuperMini verdict / action |
|---|---|---|---|
| Board target | `esp32-c3-devkitm-1`: 4 MB flash, QIO, 160 MHz | `platformio.ini:18`; `~/.platformio/platforms/espressif32/boards/esp32-c3-devkitm-1.json` | OK for SuperMini (ESP32-C3FH4, 4 MB). If a board boot-loops right after flashing, the proposed fix is `board_build.flash_mode = dio`. It is **not applied**. |
| Core / IDF | espressif32@7.1.3 → Arduino-ESP32 2.0.17, IDF 4.4. The build stops on IDF ≥ 5. | `platformio.ini:17`, `transport.cpp:14-16` | Keep the pin. |
| USB console | `Serial` is USB-Serial-JTAG CDC (`ARDUINO_USB_MODE=1`, `ARDUINO_USB_CDC_ON_BOOT=1`), 115200, 4 KB TX buffer | `platformio.ini:27-28`, `main.cpp:394-399` | Correct for SuperMini. Shows up as `/dev/ttyACM*`, VID:PID 303a:1001. |
| Monitor reset | The monitor does not toggle RTS/DTR | `platformio.ini:21-22` | Opening the monitor does not reset the board. |
| I2C (EEPROM) | SDA GPIO8, SCL GPIO9, 400 kHz | `config.h:166-167`, `eeprom.h:14`, `eeprom.cpp:53-57` | 8 and 9 are strapping pins. See 2b; acceptable with pull-ups. |
| EEPROM | AT24C256 at 0x50, 32 KB, 64-byte pages, write completion by ACK polling up to 10 ms | `eeprom.h:8-16`, `eeprom.cpp:26-45`, `eeprom.cpp:188-222` | Matches the datasheet (see 2b). |
| EEPROM missing | Halts and prints `FATAL: AT24C256 not answering on I2C (SDA 8, SCL 9, addr 0x50)` every 5 s | `main.cpp:416-418`, `main.cpp:382-388` | Useful as a wiring test. |
| Pi bridge | Node A only. UART1, RX GPIO20, TX GPIO21, 115200 8N1 | `platformio.ini:32`, `config.h:152-160`, `main.cpp:103`, `main.cpp:428-437` | 20/21 are the UART0 pins and get ROM boot text (see 2b). |
| ESP-NOW | STA mode, no modem sleep, fixed channel 6, unencrypted peers | `transport.cpp:222-230`, `transport.cpp:266`, `transport.cpp:290`, `transport.cpp:22` | OK. Nothing else on the demo site needs to be on channel 6. |
| TX power | 8.5 dBm, hard-coded, not in `config.h` | `transport.cpp:224` | This is already the usual SuperMini fix (see 2b). |
| MAC table | Hard-coded `MAC_A`…`MAC_D`. Frames from unknown MACs are dropped and counted. | `transport.cpp:24-38`, `transport.cpp:127-131`, `main.cpp:274-279` (`stats`: `unknown MAC`) | Each board must match its row (see 2b). |
| LEDs / buttons | None used by the firmware (no `pinMode`/`digitalWrite` under `src/`) | grep | The GPIO8 LED flickers with I2C traffic. That's harmless. |
| Sensors | `SimulatedSource` on every node; readings flagged `simulated` | `main.cpp:96-101`, `sensor.h:86-105` | Phase 4. |
| Power | No supply assumption in code. IDF brown-out detector on (`CONFIG_ESP32C3_BROWNOUT_DET 1`, level 7). Storage is crash-safe (A/B banks, CRC, torn-tail discard). Corrupted storage is reformatted and refilled by anti-entropy. | `…/framework-arduinoespressif32/tools/sdk/esp32c3/*/include/sdkconfig.h`; `config.h:135`; `test/storage_manager_test.cpp` | Power each board from USB. Brown-outs now show up as `reset reason: BROWNOUT`. |
| Node task | 16 KB stack, priority 3, yields 1 tick per loop | `main.cpp:55-56`, `main.cpp:376` | Check `stats` → `stack headroom` on hardware. |
| Pi side | `/dev/serial0` by default (`$EDGEKV_PORT` overrides). Serial login console must be off. | `pi/README.md:10-12`, `pi/README.md:34` | Bluetooth / Pi 5 notes in Phase 3.2. |

---

## Phase 2b: SuperMini specifics

**USB console.** The console goes to USB CDC (`platformio.ini:27-28`), so you will see it on `/dev/ttyACM*`. A crash prints its backtrace on UART0, not on USB. After a crash you will see `reset reason: PANIC …` in the banner or `stats`.

**Forcing download mode.** If a bad image locks out USB:
1. Hold BOOT (GPIO9), tap RST, release BOOT. Or hold BOOT while you plug in the cable.
2. Flash.
3. Tap RST if the board stays in the ROM.

The port keeps the same VID:PID but may come back under a different `/dev/ttyACM*` number.

**GPIO8 LED and GPIO9 BOOT on the I2C bus.**
* The LED is wired from 3V3 through a resistor to GPIO8 (active low). With SDA high it carries no current. With SDA low it draws a fraction of a mA. It doesn't load the bus enough to matter at 400 kHz. The core's own default I2C pins for this chip are 8/9 (`variants/esp32c3/pins_arduino.h:19-20`).
* Strapping: GPIO9 low at reset means download mode, and GPIO8 must be high for download boot. External pull-ups to 3.3 V keep both pins in the right state.
* Don't press BOOT while the board is running. It shorts SCL to ground in the middle of a transaction. The log is crash-safe, but don't rely on it.
* If the bus gives trouble anyway, use SDA GPIO6 / SCL GPIO7. That means editing `config.h:166-167` and the hard-coded text in `main.cpp:417`. **Not applied.**

**UART0 boot noise to the Pi.**
* The ROM prints about 300 bytes of ASCII on GPIO21 at every reset of node A.
* The Pi decoder hunts for `AA 55`, then checks the length and CRC-8 (`pi/edgekv/framing.py:59-83`). 0xAA never occurs in ASCII.
* I fed the actual ROM banner followed by one frame into `framing.Decoder`. Result: 1 frame decoded, 0 CRC errors. Existing tests cover noise, CRC errors and bogus lengths (`pi/tests/test_framing.py:24-46`).
* The firmware never starts UART0. `Serial1.begin(…, 20, 21)` (`main.cpp:429`) re-routes both pins to UART1 through the GPIO matrix. After that, UART0 output (IDF error logs, panics) no longer reaches the Pi.
* The second-stage bootloader is silent (`CONFIG_BOOTLOADER_LOG_LEVEL 0`).

**ESP-NOW range.**
* TX power is not in `config.h`. It is `WiFi.setTxPower(WIFI_POWER_8_5dBm)` at `transport.cpp:224`, already at the community-recommended 8.5 dBm.
* To make it tunable, the proposal is `constexpr std::int8_t RADIO_TX_POWER_QDBM = 34;` in `config.h`, with `esp_wifi_set_max_tx_power(config::RADIO_TX_POWER_QDBM)` after `esp_wifi_set_channel` in `transport.cpp`. Units are 0.25 dBm. **Not applied.**
* Demo layout: all boards within 0.5–2 m, antennas (the end opposite USB-C) clear of breadboard metal and cables.
* Check the link with `stats` → `esp-now tx -> X: … failed`.

**MAC table.** The file is `src/platform/transport.cpp:24-38`. The format is one block per node, in order A, B, C, D:

```cpp
constexpr std::uint8_t MAC_A[] = {
    0x14, 0x63, 0x93, 0xC5, 0xD0, 0x18
};
```

To read the MACs, plug the boards in one at a time, in A→D order, and label each one:

```bash
scripts/read_macs.sh                       # every /dev/ttyACM*, letters in port order
scripts/read_macs.sh /dev/ttyACM1          # one board
.venv/bin/pio pkg exec -p tool-esptoolpy -- esptool.py --chip esp32c3 --port /dev/ttyACM0 read_mac   # raw
ls -l /dev/serial/by-id/                   # USB serial number is usually the MAC as well
```

The STA MAC printed by esptool is the address ESP-NOW sends from. The banner reads the same one (`main.cpp:187`).

**AT24C256.**

| check | expected | code |
|---|---|---|
| address | 0x50: A0, A1, A2 to GND (module jumpers) | `eeprom.h:8` |
| WP | GND (a high WP silently blocks writes; ACK polling still passes) | n/a |
| supply | 3.3 V from the SuperMini 3V3 pin. **Never 5 V**, because the module's pull-ups would then pull SDA/SCL to 5 V | n/a |
| pull-ups | The driver relies on external pull-ups. `Wire.begin()` only enables the ~45 kΩ internal ones, too weak for 400 kHz. Check with power off: resistance from SDA to VCC and from SCL to VCC should be 2–10 kΩ. If not, add 4.7 kΩ to 3V3. | `eeprom.cpp:53-57` |
| write cycle | Datasheet tWR = 5 ms max. The driver polls for ACK every 1 ms up to 10 ms (2× margin) | `eeprom.h:15`, `eeprom.cpp:26-45` |
| pages | 64 B. Writes are split at page boundaries. Reads are 64 B chunks (Wire buffer 128). | `eeprom.h:13,16`, `eeprom.cpp:186-222`, `eeprom.cpp:97-130` |

---

## Phase 3

### 3.1 BOM: confirm with teammate

| item | qty | status |
|---|---|---|
| ESP32-C3 SuperMini | 4 (+1 spare) | required (spare nice to have) |
| AT24C256 module. Check jumpers (A0–A2, WP) and whether pull-ups are fitted. | 4 (+1 spare) | required |
| 4.7 kΩ resistors | 8 | required if the modules have no pull-ups; bring them anyway |
| Breadboards | 4 small or 2 full | required |
| Jumper wires: M-M about 40; F-F / M-F about 10 for the Pi header | — | required |
| USB-C **data** cables. Test each one: the board must enumerate. | 4 | required |
| Free USB ports for 4 boards, or a powered USB hub | — | required (hub nice to have) |
| Raspberry Pi: know the model (3/4/5/Zero 2 W). PSU, microSD with Raspberry Pi OS, SSH or keyboard+HDMI. | 1 | required |
| 3.3 V USB-UART adapter (CP2102/CH340). Reads panic output on GPIO21 of nodes B–D, and stands in for the Pi from the laptop. | 1 | nice to have, high value |
| Multimeter | 1 | nice to have |
| Sensors: DHT11, BH1750 (GY-302), PIR (AM312 or HC-SR501), sound module (LM393 / KY-038) | 1 each | nice to have (Phase 4) |
| Power bank, for a board away from the PC | 1 | nice to have |
| Tape and marker to label boards A–D with their MACs | — | required |

### 3.2 WSL setup

**Windows (PowerShell as admin, once):**

```powershell
winget install --exact dorssel.usbipd-win
usbipd list                                  # "USB JTAG/serial debug unit" 303a:1001 → note BUSID, e.g. 2-3
usbipd bind --busid 2-3                      # once per physical USB port
usbipd attach --wsl --busid 2-3 --auto-attach   # leave running; re-attaches after every reset
usbipd detach --busid 2-3                    # give the board back to Windows
```

* The ESP32-C3 re-enumerates on every reset, including the reset after each upload. Without `--auto-attach` the board disappears from WSL after every flash.
* Run one `attach --auto-attach` per board. Each needs its own PowerShell window, or use `Start-Process`.
* A BUSID belongs to a physical port, so keep each board on the same port.

**WSL (once):**

```bash
sudo usermod -aG dialout $USER            # then from Windows: wsl --shutdown
echo cdc_acm | sudo tee /etc/modules-load.d/cdc_acm.conf; sudo modprobe cdc_acm
curl -fsSL https://raw.githubusercontent.com/platformio/platformio-core/develop/platformio/assets/system/99-platformio-udev.rules \
  | sudo tee /etc/udev/rules.d/99-platformio-udev.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
source .venv/bin/activate                 # pio (or: pipx install platformio)
```

systemd/udev already run in this WSL (`/etc/wsl.conf` has `systemd=true`), and the kernel ships `cdc-acm.ko`, `usbip-core.ko` and `vhci-hcd.ko`.

**Upload quirks on WSL:**
* After upload the port disappears for 1–3 s. `pio device monitor` started immediately may fail with "could not open port"; retry.
* `ttyACM` numbers shuffle between attaches. Use `/dev/serial/by-id/...` paths with `--upload-port` and `-p`.
* "Failed to connect to ESP32-C3": use download mode (2b).

**Fallback 1: flash from native Windows.** Build in WSL, flash with esptool on Windows (detach the board from WSL first):

```bash
# WSL: stage images
mkdir -p /mnt/c/edgekv-fw && cp ~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin /mnt/c/edgekv-fw/
for e in node-a node-b node-c node-d; do mkdir -p /mnt/c/edgekv-fw/$e; cp .pio/build/$e/{bootloader,partitions,firmware}.bin /mnt/c/edgekv-fw/$e/; done
```

```powershell
py -m pip install esptool pyserial
cd C:\edgekv-fw
py -m esptool --chip esp32c3 --port COM5 read_mac
py -m esptool --chip esp32c3 --port COM5 --baud 460800 write_flash 0x0 node-a\bootloader.bin 0x8000 node-a\partitions.bin 0xe000 boot_app0.bin 0x10000 node-a\firmware.bin
py -m serial.tools.miniterm COM5 115200
```

The offsets come from the default partition table (`tools/partitions/default.csv`: otadata 0xe000, app0 0x10000). The ESP32-C3 bootloader goes at 0x0.

**Fallback 2: flash from the Pi.** `pip install platformio` on the Pi; it has native USB with no usbip. The first build downloads the riscv toolchain, so it needs internet.

**Pi UART (once):**

```bash
sudo raspi-config nonint do_serial_cons 1    # login shell on serial: off
sudo raspi-config nonint do_serial_hw 0      # UART hardware: on
# Pi 3/4/Zero 2 W: put the PL011 on GPIO14/15 (mini-UART otherwise)
echo dtoverlay=disable-bt | sudo tee -a /boot/firmware/config.txt; sudo systemctl disable hciuart
sudo reboot
ls -l /dev/serial*; groups | grep dialout
```

* On a Pi 5, GPIO14/15 is `ttyAMA0` (`dtparam=uart0=on`), and `/dev/serial0` may point at the debug connector. Pass `--port /dev/ttyAMA0`.
* On older OS images, `config.txt` is `/boot/config.txt`.
* Loopback test before wiring: jumper pin 8 to pin 10, run `python3 -m serial.tools.miniterm /dev/serial0 115200`, and check that what you type is echoed.

### 3.3 Wiring sheet

Every node (A, B, C, D):

| SuperMini pin | AT24C256 module pin |
|---|---|
| 3V3 | VCC |
| GND | GND |
| GPIO8 | SDA |
| GPIO9 | SCL |
| — | A0, A1, A2 → GND (jumpers), WP → GND |
| 3V3 via 4.7 kΩ → GPIO8, and 3V3 via 4.7 kΩ → GPIO9 | only if the module has no pull-ups |

Node A ↔ Raspberry Pi (3.3 V on both sides, no level shifter):

| Pi header | node A |
|---|---|
| pin 8, GPIO14 TXD | GPIO20 (RX) |
| pin 10, GPIO15 RXD | GPIO21 (TX) |
| pin 6, GND | GND |
| Pi 5V / 3V3 | **not connected**. Node A is powered from its USB. |

Sensor pins are in Phase 4.

### 3.4 Bring-up order

Run all `pio` commands from the repo root with `.venv` active. `P=/dev/serial/by-id/<board>`.

| # | step | command | expected | most likely failure → fix |
|---|---|---|---|---|
| 0 | MACs | `scripts/read_macs.sh` per board; label each board; compare with `transport.cpp:24-38` | 4 blocks | No port: usbipd not attached, cable is charge-only, user not in `dialout`. If the MACs differ: get approval, paste into `transport.cpp:24-38`, `make firmware`. |
| 1 | One board, no EEPROM | `pio run -e node-a -t upload --upload-port $P`; `pio device monitor -p $P` | `edge-kv firmware, node A (Pi bridge)`, `env node-a, … reset reason: …`, `MAC … matches the table entry for node A`, then `FATAL: AT24C256 not answering …` every 5 s | No output: banner missed, press RST with the monitor open. Upload fails: download mode. `MAC MISMATCH`: wrong env or table. |
| 2 | EEPROM | Wire the EEPROM, press RST. `status`, `set x 1`, `get x`, unplug/replug, `status`, `get x` | No FATAL. First boot `storage fresh format`. `SET x: UNDER_REPLICATED (served by A …)` (alone, by design D5). After the power cycle `storage recovered: 1 live`, `x = "1"`. | FATAL persists: SDA/SCL swapped, VCC not 3V3, A0–A2 not GND, no pull-ups. Writes vanish: WP high. Boot loop with EEPROM attached: SCL held low means GPIO9 is strapping into download mode; check wiring and pull-ups. |
| 3 | Two nodes (A+B) | Flash `node-b` on board B; `members` and `stats` on both | `view AB--` on both, peer `alive`, `esp-now tx -> B: N delivered, ~0 failed`, `unknown MAC 0`. `set k20 hello` on B → `served by A` and `OK` | `view A---` and `unknown MAC` rising: MAC table or env wrong (banner says which). `failed` rising: range or TX power. |
| 4 | Four nodes | Flash C, D. Pre-flight `status` on all (HARDWARE_TEST_PLAN §0) | All `ACTIVE`, `view ABCD` within about 6 s; `stats` stack headroom > 2 KB, `queue overflows 0` | One node missing from every view: its MAC row. Flapping `view:` lines: distance/RF. Bring boards closer before raising `MEMBERSHIP_TIMEOUT_MS` (`config.h:25`). |
| 5 | Pi bridge | Pi loopback first, then wire. `python3 -m edgekv --port /dev/serial0 ping`, `sensors` | `bridge is node A, round trip ~50 ms`; four readings (`simulated`) | Exit 3, no answer. Check `stats` on A: `bridge … frames 0` means nothing arrives (TX/RX swapped, no GND, serial console still on, wrong `/dev/serial0`); `crc errors` rising means wrong baud or mini-UART clock (use `disable-bt`). |
| 6 | Faults | H6 → H7 → H8 → H11 → H12a → H14 (HARDWARE_TEST_PLAN) | As in the plan | A node reboots unexpectedly: `stats` reset reason. PANIC: put the USB-UART adapter on GPIO21 (nodes B–D) to read the backtrace. |

### 3.5 Three-day schedule (HARDWARE_TEST_PLAN mapped)

E = essential for the demo, N = nice to have, D = drop first. Hours are bench time.

| when | work | tests | h |
|---|---|---|---|
| Day 0 (today, Sat 17) | WSL usbipd/dialout/udev; Pi SD card + UART config if the Pi is available; host-side sensor code once the module list is confirmed (Phase 4) | — | 1 + 3 |
| Day 1 (Sun 18) AM | Bring-up 0–4 | pre-flight, **H1 (E)**, **H2 (E)**, H3 (N) | 4 |
| Day 1 PM | Bring-up 5; first measurements (`bench 200`, `members` idle) | **H14 (E)** | 2.5 |
| Day 2 (Mon 19) AM | Core fault story | **H6 (E)**, **H7 (E)**, **H8 (E)**, **H11 (E)**, **H12a (E)**, H9 (N) | 4 |
| Day 2 PM | Sensors on hardware (Phase 4), go/no-go at 17:00 | sensor checks | 4–5 |
| Day 2 night | Unattended soak, 30 s period, real or simulated sensors | H13 (N, shortened to whatever runs overnight) | 0.5 |
| Day 3 (Tue 20) AM | Read the soak; fault-injection images if time | H4, H5, H10 (D: the sim proves them, show `cluster_test` output); H12b (D) | 1.5 |
| Day 3 PM | Demo script, 2 rehearsals, numbers table, slides | — | 4 |

* Total is about 26 h of bench time plus Day 0 prep.
* If Day 1 slips, cut H3 and H9, and keep sensors simulated (they are honestly flagged).
* The essential set (H1, H2, H6, H7, H8, H11, H12a, H14) is about 9 h.

### 3.6 Top 5 risks

| # | risk | mitigation |
|---|---|---|
| 1 | Board/MAC mismatch. Nothing talks, or nodes impersonate each other. | Step 0 before anything else. The banner flags mismatches (`main.cpp:171-243`). Label the boards. `stats` → `unknown MAC`. |
| 2 | WSL USB passthrough eats hours: 4 boards re-enumerating on every reset. | `--auto-attach`, by-id paths, one board per fixed port. Fall back to Windows esptool or flashing from the Pi (3.2) after 30 min of trouble. |
| 3 | The integrated firmware has never run on a board (`STATUS.md:67-70`): stack, timing or USB-output bugs, and panics not visible on USB. | One board first (step 1). Check `reset reason` and stack headroom in `stats`. USB-UART adapter on GPIO21. Budget half of Day 1 PM as slack. The sim (`build/host/edge_sim`) is the demo fallback for the distributed-systems part. |
| 4 | Pi UART misconfigured (console on, mini-UART, Pi 5 naming), so the bridge is silent. | Loopback test first. `stats` bridge counters on A. The USB-UART adapter on the laptop can run `python3 -m edgekv --port /dev/ttyUSB0 ping` against node A's GPIO20/21, which takes the Pi out of the equation. |
| 5 | Scope: sensors (and a dashboard) eat validation time. | Sensors are gated: host code before the boards arrive, hardware work only after the essential tests pass, go/no-go Day 2 17:00. Simulated fallback per node (Phase 4). No dashboard (DEMO_PLAN phase 3 is out of scope); use `watch --csv`. |

Also watch for: with USB connected but no monitor open, the core can block up to 100 ms per console write (`framework-arduinoespressif32/cores/esp32/HWCDC.cpp:81`, `:440-466`). If views flap only when monitors are closed, run `log off` on idle nodes.

---

## Phase 4: real sensors (plan only; waiting for the module list)

### Proposed mapping (confirm)

| node | module | kind | interface | pin | notes |
|---|---|---|---|---|---|
| A (bridge) | DHT11 | TEMPERATURE (humidity is a stretch goal) | single-wire, open drain | GPIO4 | 3.3 V supply. 4.7–10 kΩ pull-up to 3V3 (most modules have one). At most 1 read/s. |
| B | PIR (AM312 / HC-SR501) | MOTION (new) | digital, active high | GPIO4 | AM312 runs at 3.3 V. HC-SR501 can take 5 V on VCC, its output is 3.3 V. |
| C | sound module (LM393 comparator) | SOUND (new) | digital DO (AO optional) | DO GPIO4, AO GPIO3 (ADC1) | **Power from 3V3**, so DO never exceeds 3.3 V. Avoid GPIO5 for AO (ADC2 conflicts with Wi-Fi). |
| D | BH1750 (GY-302) | LIGHT | I2C 0x23 on the EEPROM bus | GPIO8/9 | ADDR → GND. Power from 3V3. Pull-ups in parallel with the EEPROM's (≈3 kΩ total) is fine. |

* All pins avoid 2/8/9 (strapping) and 20/21 (UART). GPIO8/9 are reused for I2C only.
* GPIO4–7 double as JTAG pins, but they are free while debugging goes through USB-JTAG.
* Spare pins: 3, 5, 6, 7, 10.

**Why this mapping and not the one in `STATUS.md:289-294`** (A DHT11, B BH1750, C PIR, D sound): A stays temperature and D stays light, which are the current simulated kinds (`sensor.cpp:67-75`). Those two need no kind or Pi changes. Only B and C change kind.

### Drivers

| module | choice | blocking behaviour |
|---|---|---|
| DHT11 | Pinned library `adafruit/DHT sensor library` + `adafruit/Adafruit Unified Sensor`. Pin exact versions in `lib_deps` (`pio pkg show …`; 1.4.6 / 1.1.14 are the latest I know of). A hand-written driver is a 60-line fallback with the same timing. | One read takes about 25 ms: an 18–20 ms start pulse via `delay()` (yields to FreeRTOS), then about 5 ms with interrupts off while it reads 40 bits. That happens once per sample period (30 s) in the node task. Heartbeats are every 500 ms and the dead timeout is 2 s (`config.h:22-25`), so the stall is harmless. The radio MAC ACKs in hardware; RX callbacks queue (`transport.cpp:154`). Never read more often than every 2 s. |
| BH1750 | `claws/BH1750` (pinned), `begin(CONTINUOUS_HIGH_RES_MODE, 0x23, &Wire)`, called after `eeprom_chip.begin()` | In continuous mode `readLightLevel()` is one 2-byte read, about 0.1 ms. Don't use one-time mode (120–180 ms delay). Don't call `Wire.begin()` again. Single node task, so no mutex (DECISIONS D2, `docs/DECISIONS.md:52-63`). |
| PIR, sound | Hand-written, about 30 lines: `attachInterrupt(pin, isr, RISING)`, where an `IRAM_ATTR` ISR increments a `volatile uint32_t`. No library. | Non-blocking. `read()` swaps the counter out and reports events per period. |

### Plugging into the sensor interface

* Interface: `sensor::Source` (`include/sensor.h:74-84`: `begin`, `kind`, `simulated`, `read(now, value_milli)`). Constructed at `src/platform/main.cpp:96-101` (`sensor_source`, `sampler`).
* The sampler reads the source once per period and writes `s/<node>/<kind>` and `h/<node>/<kind>/<seq%4>` (`src/sensor.cpp:262-330`). Nothing there changes.

Changes needed:
1. `include/sensor.h:28-33`: add `MOTION = 5` (milli-events per period) and `SOUND = 6`. `src/sensor.cpp:33` `valid_kind`, `:43-61` `kind_key` / `kind_unit` (`"motion"`, `"sound"`, unit `"events"`).
2. `src/sensor.cpp:67-75` `kind_for_node`: B → MOTION, C → SOUND. This is also used by the console `sensors` command (`src/console.cpp:405`) and the sim (`sim/cluster.cpp:282`).
3. `src/sensor.cpp:163-180` `SimulatedSource::read`: add MOTION and SOUND profiles (small non-negative counts), so the fallback and the sim cover the new kinds.
4. New, platform-independent: an `EventSource` (counter in, count per period out, with an injectable count function) and a `FallbackSource(real, simulated)` that serves the simulated source if `real.begin()` fails or `read()` fails N times in a row. Today `main.cpp:426` ignores the return value of `sampler.begin()`.
5. New, platform-only: `src/platform/sensors_dht.cpp`, `sensors_bh1750.cpp`, `sensors_event.cpp`, plus `lib_deps` in `platformio.ini`.
6. Per-node switch, a single home (rule R8): `config.h` gets `constexpr std::uint8_t REAL_SENSOR_NODES = 0x0;` (bit n-1 = node n uses its real sensor). `main.cpp` picks the real source, wrapped in `FallbackSource`, when the bit is set. Default 0 means everything stays simulated until flipped.
7. Pi (`pi/edgekv/reading.py:21` `KINDS`: add `5: ("motion","events"), 6: ("sound","events")`; `:24` `NODE_SENSORS`: B → motion, C → sound). `client.py:159` iterates `NODE_SENSORS`, so no change there. Check `cli.py` formatting of event counts.

### Host coverage

* `EventSource`, `FallbackSource`, the new kinds, key names and encoding are all platform-independent, so `make check` covers them:
  * extend `test/sensor_test.cpp`;
  * extend the sensor section of `test/cluster_test.cpp`, where B and C now sample MOTION and SOUND through `SimulatedSource`;
  * Pi `ReadingTest` vectors (`pi/tests/test_protocol.py`).
* `pi/tests/test_sim_integration.py:103` only asserts A is `temp`, which is unchanged.
* The DHT11, BH1750 and ISR drivers can't run on the host; `make firmware` covers them at compile time only.

### Estimate

| work | h | needs boards |
|---|---|---|
| Kinds, `SimulatedSource` profiles, `kind_for_node`, `EventSource`, `FallbackSource`, config switch, host tests | 2.5 | no |
| Pi `reading.py` + tests | 0.5 | no |
| DHT11 driver + wiring check on A | 1.5 | yes |
| BH1750 on the shared bus (EEPROM still OK: `set`/`get`, `stats` storage errors 0) on D | 1.5 | yes |
| PIR + sound ISR drivers on B, C | 1.5 | yes |
| 4-node integration, `sensor period 5000` demo check, logs | 1.5 | yes |
| **total** | **9** (3 off-board, 6 on-board) | |

* The 3 off-board hours go into Day 0/Day 1 evening.
* The 6 on-board hours go into Day 2 PM, after the essential H-tests.
* If 17:00 Day 2 arrives without a working sensor, leave `REAL_SENSOR_NODES = 0` and demo the simulated readings.
* EEPROM wear at a 5 s demo period is about 6× the 30 s rate (`config.h:141-145`): fine for days, so set the period back to 30 s afterwards.

**Not started. Waiting for confirmation of the module list and the node mapping.**

---

## Proposed changes not applied (need approval)

| change | file | when |
|---|---|---|
| Paste the real MACs | `src/platform/transport.cpp:24-38` | if step 0 shows a difference |
| `board_build.flash_mode = dio` | `platformio.ini` `[env]` | only if a SuperMini boot-loops after flashing |
| I2C on GPIO6/7 | `config.h:166-167`, `main.cpp:417` | only if 8/9 misbehave |
| `RADIO_TX_POWER_QDBM` in config | `config.h`, `transport.cpp:224` | if range needs tuning |
| Phase 4 sensor work | see Phase 4 | after the module list is confirmed |
