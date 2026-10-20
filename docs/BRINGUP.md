# Hardware bring-up (4× ESP32-C3 SuperMini, AT24C256, Raspberry Pi)

Written 2026-10-17, the day before the hardware arrives. Plan: Day 1 = Sun 18 Oct,
Day 2 = Mon 19 Oct, Day 3 = Tue 20 Oct (demo). Line numbers refer to branch
`hw-bringup` after the sensor commits.

## Changes already made (approved)

| change | where | what it does |
|---|---|---|
| A. Boot identity | `src/platform/main.cpp:146-266`, called at `main.cpp:506` and from `stats` (`main.cpp:283`); `transport::configured_mac()` in `include/transport.h:74-75` and `src/platform/transport.cpp:188-191`; `EDGE_ENV_NAME` set by `platformio.ini:29-30` | Prints the env name, build time, `esp_reset_reason()` as text, and the board's STA MAC. Says whether the MAC matches the table entry for `NODE_ID`. On a mismatch it prints a `MAC MISMATCH` block and, if the MAC belongs to another node, which env to flash instead. |
| B. MAC reader | `scripts/read_macs.sh` | Runs `esptool.py read_mac` on each `/dev/ttyACM*` (or the ports you list) and prints `MAC_A`…`MAC_D` blocks in the format of `transport.cpp:24-38`. |

* The build time is the compile time of `main.cpp`. PlatformIO rebuilds that file only when it changes, so after edits to other files the time can be older than the image.
* The banner prints about 1.5 s after reset (`main.cpp:497`). Under WSL the port usually re-attaches later than that, so you may miss it. Type `stats` to see the identity lines again. Without an EEPROM `stats` does not answer (the board halts before the console starts); press RST with the monitor open instead.
* The MAC table itself is unchanged.

---

## Phase 2: hardware assumptions in the code

| area | assumption | source | SuperMini verdict / action |
|---|---|---|---|
| Board target | `esp32-c3-devkitm-1`: 4 MB flash, QIO, 160 MHz | `platformio.ini:18`; `~/.platformio/platforms/espressif32/boards/esp32-c3-devkitm-1.json` | OK for SuperMini (ESP32-C3FH4, 4 MB). If a board boot-loops right after flashing, the proposed fix is `board_build.flash_mode = dio`. It is **not applied**. |
| Core / IDF | espressif32@7.1.3 → Arduino-ESP32 2.0.17, IDF 4.4. The build stops on IDF ≥ 5. | `platformio.ini:17`, `transport.cpp:14-16` | Keep the pin. |
| USB console | `Serial` is USB-Serial-JTAG CDC (`ARDUINO_USB_MODE=1`, `ARDUINO_USB_CDC_ON_BOOT=1`), 115200, 4 KB TX buffer | `platformio.ini:27-28`, `main.cpp:488-493` | Correct for SuperMini. Shows up as `/dev/ttyACM*`, VID:PID 303a:1001. |
| Monitor reset | No `monitor_rts`/`monitor_dtr` settings, so the monitor leaves both lines asserted | `platformio.ini:21-22` | Opening the monitor does not reset the board (checked on node D, Sun 18). The earlier `monitor_rts = 0`/`monitor_dtr = 0` **did** reset it on every open: DTR goes low before RTS, and that moment is the USB-Serial-JTAG reset condition (BRINGUP_LOG problem 1). The first open after a while shows a burst of old output buffered on the board (4 KB TX buffer). |
| I2C (EEPROM) | SDA GPIO8, SCL GPIO9, 400 kHz | `config.h:166-167`, `eeprom.h:14`, `eeprom.cpp:57-61` | 8 and 9 are strapping pins. See 2b; acceptable with pull-ups. |
| EEPROM | AT24C256 at 0x50, 32 KB, 64-byte pages, write completion by ACK polling up to 10 ms | `eeprom.h:8-16`, `eeprom.cpp:28-47`, `eeprom.cpp:191-232` | Matches the datasheet (see 2b). |
| EEPROM missing | Halts and prints `FATAL: AT24C256 not answering on I2C (SDA 8, SCL 9, addr 0x50)` every 5 s | `main.cpp:514-516`, `main.cpp:476-482` | Useful as a wiring test. |
| Pi bridge | Node A only. UART1, RX GPIO20, TX GPIO21, 115200 8N1 | `platformio.ini:32`, `config.h:152-160`, `main.cpp:117`, `main.cpp:535-545` | 20/21 are the UART0 pins and get ROM boot text (see 2b). |
| ESP-NOW | STA mode, no modem sleep, fixed channel 6, unencrypted peers | `transport.cpp:222-230`, `transport.cpp:266`, `transport.cpp:290`, `transport.cpp:22` | OK. Nothing else on the demo site needs to be on channel 6. |
| TX power | 8.5 dBm, hard-coded, not in `config.h` | `transport.cpp:224` | This is already the usual SuperMini fix (see 2b). |
| MAC table | Hard-coded `MAC_A`…`MAC_D`. Frames from unknown MACs are dropped and counted. | `transport.cpp:24-38`, `transport.cpp:127-131`, `main.cpp:303-308` (`stats`: `unknown MAC`) | Each board must match its row (see 2b). |
| LEDs / buttons | None used by the firmware (no `pinMode`/`digitalWrite` under `src/`) | grep | The GPIO8 LED flickers with I2C traffic. That's harmless. |
| Sensors | Real drivers for DHT11 / AM312 / LM393 / BH1750 exist but are off: `REAL_SENSOR_NODES = 0x00`, so every node runs `SimulatedSource` and readings are flagged `simulated` | `config.h:151`, `main.cpp:101-118`, `src/platform/sensors.cpp` | Phase 4. |
| Power | No supply assumption in code. IDF brown-out detector on (`CONFIG_ESP32C3_BROWNOUT_DET 1`, level 7). Storage is crash-safe (A/B banks, CRC, torn-tail discard). Corrupted storage is reformatted and refilled by anti-entropy. | `…/framework-arduinoespressif32/tools/sdk/esp32c3/*/include/sdkconfig.h`; `config.h:135`; `test/storage_manager_test.cpp` | Power each board from USB. Brown-outs now show up as `reset reason: BROWNOUT`. |
| Node task | 16 KB stack, priority 3, yields 1 tick per loop | `main.cpp:59-61`, `main.cpp:453` | Check `stats` → `stack headroom` on hardware. |
| Pi side | `/dev/serial0` by default (`$EDGEKV_PORT` overrides). Serial login console must be off. | `pi/README.md:10-12`, `pi/README.md:34` | Bluetooth / Pi 5 notes in Phase 3.2. |

---

## Phase 2b: SuperMini specifics

**USB console.** The console goes to USB CDC (`platformio.ini:27-28`), so you will see it on `/dev/ttyACM*`. A crash prints its backtrace on UART0, not on USB. After a crash you will see `reset reason: PANIC …` in the banner or `stats`.

**Echo guard.** For 1 s after boot the console discards what it receives and then prints `console: ignored N character(s) received while the port was opening`. The host's tty can echo the board's buffered output back before it switches to raw mode (BRINGUP_LOG problems 5 and 10). Opening the port later is not detected, so **send one empty line first** after opening a console: it runs any echoed fragment as a harmless unknown command.

**Forcing download mode.** If a bad image locks out USB:
1. Hold BOOT (GPIO9), tap RST, release BOOT. Or hold BOOT while you plug in the cable.
2. Flash.
3. Tap RST if the board stays in the ROM.

The port keeps the same VID:PID but may come back under a different `/dev/ttyACM*` number.

**GPIO8 LED and GPIO9 BOOT on the I2C bus.**
* The LED is wired from 3V3 through a resistor to GPIO8 (active low). With SDA high it carries no current. With SDA low it draws a fraction of a mA. It doesn't load the bus enough to matter at 400 kHz. The core's own default I2C pins for this chip are 8/9 (`variants/esp32c3/pins_arduino.h:19-20`).
* Strapping: GPIO9 low at reset means download mode, and GPIO8 must be high for download boot. External pull-ups to 3.3 V keep both pins in the right state.
* Don't press BOOT while the board is running. It shorts SCL to ground in the middle of a transaction. The log is crash-safe, but don't rely on it.
* If the bus gives trouble anyway, use SDA GPIO6 / SCL GPIO7. That means editing `config.h:166-167` and the hard-coded text in `main.cpp:515`. **Not applied.**

**UART0 boot noise to the Pi.**
* The ROM prints about 300 bytes of ASCII on GPIO21 at every reset of node A.
* The Pi decoder hunts for `AA 55`, then checks the length and CRC-8 (`pi/edgekv/framing.py:59-83`). 0xAA never occurs in ASCII.
* I fed the actual ROM banner followed by one frame into `framing.Decoder`. Result: 1 frame decoded, 0 CRC errors. Existing tests cover noise, CRC errors and bogus lengths (`pi/tests/test_framing.py:24-46`).
* The firmware never starts UART0. `Serial1.begin(…, 20, 21)` (`main.cpp:536`) re-routes both pins to UART1 through the GPIO matrix. After that, UART0 output (IDF error logs, panics) no longer reaches the Pi.
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

The STA MAC printed by esptool is the address ESP-NOW sends from. The banner reads the same one (`main.cpp:216`).

**AT24C256.**

| check | expected | code |
|---|---|---|
| address | 0x50: A0, A1, A2 to GND (module jumpers) | `eeprom.h:8` |
| ID block | A scan shows the chip at **0x50 and 0x58** (seen on every module, Mon 19). 0x58 is the chip's ID/security block. The driver must **never write 0x58**: on parts with a lockable ID page a write there may lock it permanently. A `static_assert` in `eeprom.h` keeps the driver at 0x50-0x57, and one in `config.h` keeps the BH1750 off 0x50-0x5F (with ADDR high it would be 0x5C) | `eeprom.h` (end), `config.h` (end) |
| WP | GND (a high WP silently blocks writes; ACK polling still passes) | n/a |
| supply | 3.3 V from the SuperMini 3V3 pin. **Never 5 V**, because the module's pull-ups would then pull SDA/SCL to 5 V | n/a |
| wiring | Wire **by the module's silkscreen labels, not by header position**: the pin order differs between modules and from the SuperMini. A first direct wiring by position gave the FATAL and no device at any address (BRINGUP_LOG problem 4). | n/a |
| pull-ups | Our modules have them fitted: 9.84 kΩ on SDA and SCL (measured). The driver relies on external pull-ups. `Wire.begin()` only enables the ~45 kΩ internal ones, too weak for 400 kHz. Check with power off: resistance from SDA to VCC and from SCL to VCC should be 2–10 kΩ. If not, add 4.7 kΩ to 3V3. | `eeprom.cpp:57-61` |
| write cycle | Datasheet tWR = 5 ms max. The driver polls for ACK every 1 ms up to 10 ms (2× margin) | `eeprom.h:15`, `eeprom.cpp:28-47` |
| pages | 64 B. Writes are split at page boundaries. Reads are 64 B chunks (Wire buffer 128). | `eeprom.h:13,16`, `eeprom.cpp:191-232`, `eeprom.cpp:105-156` |

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
| — | Match pins **by the silkscreen labels** on both boards, not by header position |
| — | **`format yes` on every EEPROM module at first install**, before the node joins the cluster. A reused module keeps records from earlier tests, and the cluster accepts them as real data (BRINGUP_LOG problem 7, DECISIONS D11) |
| 3V3 via 4.7 kΩ → GPIO8, and 3V3 via 4.7 kΩ → GPIO9 | only if the module has no pull-ups |

Node A ↔ Raspberry Pi (3.3 V on both sides, no level shifter):

| Pi header | node A |
|---|---|
| pin 8, GPIO14 TXD | GPIO20 (RX) |
| pin 10, GPIO15 RXD | GPIO21 (TX) |
| pin 6, GND | GND |
| Pi 5V / 3V3 | **not connected**. Node A is powered from its USB. |

Sensors (each node has one; `config.h:159-165`):

| node | module pin | SuperMini pin | notes |
|---|---|---|---|
| A (bridge) | GY-302 VCC / GND / SDA / SCL / ADDR | 3V3 / GND / **GPIO8** / **GPIO9** / GND | Same bus as A's EEPROM, address 0x23. **3V3 only**: its pull-ups may go to VCC. Its driver never disables interrupts, so the Pi link is unaffected. |
| B | AM312 VCC / GND / OUT | 3V3 / GND / **GPIO4** | 3.3 V supply is fine; no pots |
| C | LM393 VCC / GND / DO | 3V3 / GND / **GPIO4** | **3V3 only**, so DO never exceeds 3.3 V. DO is usually active-low. |
| D | DHT11 module VCC / GND / DATA | 3V3 / GND / **GPIO4** | 3-pin module has its own pull-up |

GPIO4 is not a strapping pin, not USB (18/19) and not the bridge UART (20/21). `config.h:262-272` refuses any pin other than 0–10 minus 2/8/9 at compile time. Spare pins: 3, 5, 6, 7, 10.

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
| Day 0 (today, Sat 17) | WSL usbipd/dialout/udev; Pi SD card + UART config if the Pi is available. Host-side sensor code: **done** (Phase 4). | — | 1 |
| Day 1 (Sun 18) AM | Bring-up 0–4 | pre-flight, **H1 (E)**, **H2 (E)**, H3 (N) | 4 |
| Day 1 PM | Bring-up 5; first measurements (`bench 200`, `members` idle) | **H14 (E)** | 2.5 |
| Day 2 (Mon 19) AM | Core fault story | **H6 (E)**, **H7 (E)**, **H8 (E)**, **H11 (E)**, **H12a (E)**, H9 (N) | 4 |
| Day 2 PM | Sensors on hardware, one module at a time (Phase 4.6), go/no-go at 17:00 | per-module checks | 6 |
| Day 2 night | Unattended soak, 30 s period, real or simulated sensors | H13 (N, shortened to whatever runs overnight) | 0.5 |
| Day 3 (Tue 20) AM | Read the soak; fault-injection images if time | H4, H5, H10 (D: the sim proves them, show `cluster_test` output); H12b (D) | 1.5 |
| Day 3 PM | Demo script, 2 rehearsals, numbers table, slides | — | 4 |

* Total is about 23.5 h, Day 0 prep included. Track planned vs actual in `docs/BRINGUP_LOG.md`.
* If Day 1 slips, cut H3 and H9, and keep sensors simulated (they are honestly flagged).
* The essential set (H1, H2, H6, H7, H8, H11, H12a, H14) is about 9 h.

### 3.6 Top 5 risks

| # | risk | mitigation |
|---|---|---|
| 1 | Board/MAC mismatch. Nothing talks, or nodes impersonate each other. | Step 0 before anything else. The banner flags mismatches (`main.cpp:190-272`). Label the boards. `stats` → `unknown MAC`. |
| 2 | WSL USB passthrough eats hours: 4 boards re-enumerating on every reset. | `--auto-attach`, by-id paths, one board per fixed port. Fall back to Windows esptool or flashing from the Pi (3.2) after 30 min of trouble. |
| 3 | The integrated firmware has never run on a board (`STATUS.md:67-70`): stack, timing or USB-output bugs, and panics not visible on USB. | One board first (step 1). Check `reset reason` and stack headroom in `stats`. USB-UART adapter on GPIO21. Budget half of Day 1 PM as slack. The sim (`build/host/edge_sim`) is the demo fallback for the distributed-systems part. |
| 4 | Pi UART misconfigured (console on, mini-UART, Pi 5 naming), so the bridge is silent. | Loopback test first. `stats` bridge counters on A. The USB-UART adapter on the laptop can run `python3 -m edgekv --port /dev/ttyUSB0 ping` against node A's GPIO20/21, which takes the Pi out of the equation. |
| 5 | Scope: sensors (and a dashboard) eat validation time. | The host-side sensor code is already done. Hardware work starts only after the essential tests pass, with a per-module go/no-go at Day 2 17:00. Each node falls back to its simulated sensor (Phase 4). No dashboard (DEMO_PLAN phase 3 is out of scope); use `watch --csv`. |

Also watch for: with USB connected but no monitor open, the core can block up to 100 ms per console write (`framework-arduinoespressif32/cores/esp32/HWCDC.cpp:81`, `:440-466`). If views flap only when monitors are closed, run `log off` on idle nodes.

---

## Phase 4: real sensors

### 4.1 State

The pre-hardware part is done on branch `hw-bringup`. `make check` is green, and all 8 images build with the switch at `0x00` (shipped) and at `0x0F` (checked once).

| commit | content |
|---|---|
| `9b9229e` | MOTION / SOUND kinds end to end: firmware, reading format v2, console, Pi client, CLI, CSV |
| `65fa686` | Platform-independent sensor logic + host fakes, `REAL_SENSOR_NODES` switch, pins and timings in `config.h` |
| `6533991` | Pi `--raw-log FILE` |
| `55c6188` | Drivers (`src/platform/sensors.cpp`), I2C bus lock. Off until a node's bit is set. |

No driver has run on a board yet. Each module is enabled only after it was checked on its own (4.6).

### 4.2 Modules (mapping approved)

| node | module | interface | pin (`config.h`) | reading |
|---|---|---|---|---|
| A (bridge) | GY-302 BH1750 | I2C 0x23 on the EEPROM bus | `BH1750_I2C_ADDRESS` (`:165`), GPIO8/9 | `s/A/light`, lux |
| B | AM312 mini PIR | digital, push-pull, high ~2 s per detection | `PIR_PIN = 4` (`:161`) | `s/B/motion`: detections per window + seconds since last motion; `settling` after power-on |
| C | LM393 sound, **DO only** (confirmed DO-only; no AO work) | digital comparator output, active level in `SOUND_ACTIVE_HIGH` (`:182`, default `false` = active-low) | `SOUND_DO_PIN = 4` (`:162`) | `s/C/sound`: events per window + % of window active |
| D | DHT11 3-pin module | single-wire, open drain, pull-up on module | `DHT11_PIN = 4` (`:163`) | `s/D/temp`, degC; `STALE` / `ERROR` when it stops reading |

The bridge node A gets the BH1750 because its driver never disables interrupts. The DHT11 library turns interrupts off for ~5 ms per read, so it goes on D (decided 2026-10-17).

**LM393:** DO only (decided). No AO / peak-to-peak work unless requested.

### 4.3 Per-module design

**DHT11 (D).**
* Driver: `adafruit/DHT sensor library@1.4.7` (+ `Adafruit Unified Sensor@1.1.15`), pinned in `platformio.ini:33-36`.
* It runs in its own task, `Dht11Input` (`sensors.cpp:45-111`): priority 1 (the node task is 3), one read every `DHT11_READ_INTERVAL_MS = 2000` (`config.h:168`). A `static_assert` keeps that at ≥ 1000 ms, i.e. at most 1 Hz.
* A read takes about 25 ms. 20 ms of that is `delay()`, which only sleeps the DHT task. The other ~5 ms has **interrupts disabled**: the library's `InterruptLock` (`DHT.cpp:285`) is `portDISABLE_INTERRUPTS()` on this core.
* So the node task never waits on the DHT. Radio frames arriving in those ~5 ms are held by the Wi-Fi hardware and the RX queue.
* The node task only calls `take()`, which swaps counters under a spinlock.
* `LatestValueSource` (`sensor_sources.h:133`) keeps the last good value through failed reads and counts them. `status` shows them as `read failures`.
* Staleness: after `DHT_STALE_MS = 10000` (`config.h:172`) without a good read, readings carry the `STALE` flag and keep the last good value. With no good read since boot they carry `ERROR` with value 0. Both show in `sensors` / `get`, the Pi text output and the CSV `status` column. The DHT11 is never replaced by simulated values (`fallback_allowed()` is false).

**AM312 (B).**
* `PirInput` (`sensors.cpp:118`): `INPUT_PULLDOWN`, so an unplugged sensor reads as "no motion".
* `MotionSource` (`sensor_sources.h:42`) polls every `PIR_POLL_INTERVAL_MS = 100` (`config.h:177`), well inside the ~2 s pulse.
* Value: rising edges per window. Aux: seconds from the last time OUT was seen high to the sample.
* The Pi shows "last motion N s ago", adding the sample's age.
* `PIR_SETTLE_MS = 30000` (`config.h:178`) counts from the first poll, which is about power-on. Until then every reading carries the `settling` flag with value 0, and input is ignored.
* **Measure the settle time:** set `PIR_SETTLE_MS = 0` temporarily, power-cycle with nobody in view, and note when spurious detections stop. Use that plus a margin.

**LM393 sound (C).**
* `SoundInput` (`sensors.cpp:135-160`): an `IRAM_ATTR` ISR on the edge into the active level (FALLING when active-low). It only increments a 32-bit counter: no logging, no calls.
* `SoundSource` (`sensor_sources.h:78`) reads counter differences on every node-task iteration, so wrap-around is handled. Edges closer than `SOUND_EVENT_GAP_MS = 150` (`config.h:185`) form one event, because a clap makes the comparator chatter.
* "% of window active" is measured by sampling the DO level on each iteration (about 1 ms).
* DO stuck active with no edges shows as 100 % active, 0 events. That means the knob is too sensitive.

**BH1750 (A, the bridge).**
* `Bh1750Source` (`sensors.cpp:171`): power-on, then continuous high-res mode (`0x01`, `0x10`) at begin. A read is one 2-byte transfer, and lux = raw / 1.2.
* No library: three I2C transfers. That also lets every transfer hold the bus lock.

**Shared bus.**
* `i2c_bus::Lock` (`src/platform/i2c_bus.h`) is held by every EEPROM read and write (`eeprom.cpp`) for the whole transfer, including write-cycle ACK polling. Every BH1750 transfer holds it too.
* So a sensor read can't start inside an EEPROM write cycle or between the address phase and data phase of an EEPROM read.
* Today both users run in the node task, so the lock never waits. It enforces DECISIONS D2 if that ever changes.

### 4.4 How it plugs in

| piece | where |
|---|---|
| Kinds `MOTION = 5`, `SOUND = 6`, `has_aux`, reading v2 (24 B: v1 + i32 aux), flag `SETTLING` | `include/sensor.h:29-73`, `src/sensor.cpp` (codec, `kind_for_node` at `:77`) |
| Source interface: `Sample`, `poll()`, `failures()` | `include/sensor.h:106-130` |
| Logic: `MotionSource`, `SoundSource`, `LatestValueSource`, `FallbackSource`, `real_sensor_enabled()` | `include/sensor_sources.h`, `src/sensor_sources.cpp` |
| Per-node switch | `config.h:151` `REAL_SENSOR_NODES` (A 0x01, B 0x02, C 0x04, D 0x08); fallback after `SENSOR_FALLBACK_AFTER_FAILURES = 3` (`:156`) |
| Wiring in the firmware | `main.cpp:101-118` (FallbackSource around `platform_sensors::real_source()`), boot line `sensor <kind>: hardware / simulated` (`main.cpp:527-533`) |
| Pi | `pi/edgekv/reading.py:31-42` (kinds, v2 decode, `NODE_SENSORS`), `cli.py:18` (CSV gets trailing columns `last_motion_s, active_pct, settling`), `--raw-log` (`pi/edgekv/rawlog.py`) |

### 4.5 Host coverage (`make check`)

* `test/sensor_test.cpp`: v1/v2 codec byte layout, formatter, kinds, simulated event sources over 24 h.
* `test/sensor_sources_test.cpp` (new):
  * PIR settling, detections, ages, 100 ms polling;
  * sound chatter grouping, duty, counter wrap, stuck-active;
  * DHT keep-last-good and failure count;
  * fallback switching both ways;
  * the switch mask, including the shipped all-simulated default.
* `test/cluster_test.cpp` `SENSORS` (new): all four logics on a 4-node sim over fake inputs driven by cluster time, read back through the cluster. Three deliberate mutations of the scenario made it fail as expected.
* Pi: `test_protocol` (v2 decode, same bytes as C++), `test_cli` (text and CSV), `test_rawlog` (discard hook, merging, client with ROM text before every answer).
* Drivers (`src/platform/sensors.cpp`, `i2c_bus.cpp`) are compile-checked only.

### 4.6 Enabling one module (do one at a time)

1. Wire it as in 3.3, on its own board (one node alone is fine: it goes ACTIVE, and writes report `UNDER_REPLICATED`).
2. Set that node's bit in `config.h:151` (A `0x01`, B `0x02`, C `0x04`, D `0x08`), `pio run -e node-b -t upload --upload-port $P`. The other nodes only read their own bit, so they don't need reflashing.
3. The boot output must say `sensor motion: hardware`. If it says `hardware did not start, using the SIMULATED source`, `begin()` failed.
4. Run `sensor period 5000`, then use `status` (sensor line `(hardware)`, `read failures`) and `get s/<node>/<kind>` (`s/A/light`, `s/B/motion`, `s/C/sound`, `s/D/temp`).

| module | stimulus | expected | likely failure → fix |
|---|---|---|---|
| DHT11 (D) | breathe on it; then pull DATA for 15 s | temp rises within a few samples; `read failures` stays near 0. With DATA pulled: `STALE` after 10 s, then back to normal when reconnected. | `ERROR (no good temp read since boot)`: DATA not on GPIO4, or the module is powered from the wrong pin. |
| AM312 (B) | stay still for `PIR_SETTLE_MS`, then wave | `settling` first; then `1 events, last motion 0 s before the sample`; quiet windows show age growing | Detections with nobody there: settle time too short (measure, 4.3). Never any: OUT not on GPIO4. |
| LM393 (C) | set the knob so the DO LED is off in a quiet room; clap twice | `2 events, x % active` | 100 % active: knob too sensitive or `SOUND_ACTIVE_HIGH` wrong. 0 events but LED flashes: wrong edge (flip `SOUND_ACTIVE_HIGH`). |
| BH1750 (A) | cover / torch | lux drops / rises; `set`/`get` and `bench 50` still OK; `stats` storage errors 0 | `hardware did not start`: ADDR not low, or SDA/SCL swapped. EEPROM FATAL appearing after adding it: bus loaded (check pull-ups, 3V3). |

5. Leave the period at 5 s for the demo (EEPROM wear about 6× the 30 s rate; fine for days), then set it back to 30 s.
6. Commit the bit flip with the log of the check, e.g. `docs/test-logs/sensor-B.log`.

### 4.7 Estimate and slot

| work | h | status |
|---|---|---|
| Kinds, logic, fakes, tests, Pi, raw-log, drivers | — | done |
| DHT11 on D, including the radio check (4.8) | 1.5 | Day 2 PM |
| AM312 on B, including settle measurement | 1.5 | Day 2 PM |
| LM393 on C, including knob setting | 1 | Day 2 PM |
| BH1750 on A, including EEPROM coexistence (`bench 50` while sampling at 1 s) | 1 | Day 2 PM |
| All four together, 30 min run, logs | 1 | Day 2 PM |
| **total remaining** | **6** | after the essential H-tests; go/no-go 17:00 |

Go/no-go is per module. Any module not confirmed by 17:00 keeps its bit clear and stays simulated (flagged `simulated` everywhere), so it can't break the demo.


### 4.8 Check: does the DHT11 disturb the radio? (`bench 50` on D, bit off vs on)

The DHT11 library disables interrupts for ~5 ms per read, every 2 s (4.3). This check measures whether that costs D any radio retries or latency. Do it with all four nodes up and ACTIVE, D's DHT11 wired, and the other nodes' sensor bits as they are.

`bench` prints only ok/failed and min/avg/max latency (`console.cpp:741-756`, summary at `:763`). Retries show up as deltas in `stats`:
* `client … attempts` on D: one attempt per op unless an attempt timed out.
* `radio … tx` and `(failed …)` on D: retransmissions and MAC-level send failures.
* `server … dedup replays` on the primaries: retransmissions that reached them.

| run | D's bit (`config.h:151`) | steps on D |
|---|---|---|
| 1 | clear (`0x00` or without `0x08`), flash `node-d` | `sensor period 1000`, wait 10 s, `stats` → note; `bench 50` ×3 → note each line; `stats` → note |
| 2 | set (`… | 0x08`), flash `node-d`; boot line says `sensor temp: hardware` | the same steps; also check `status` shows the DHT11 reading (no `ERROR`) |

On A, B and C, run `stats` before and after each run for `dedup replays`. Save both runs as `docs/test-logs/dht-radio-off.log` / `-on.log`.

| metric (per 150 SETs) | bit off | bit on | pass if |
|---|---|---|---|
| ok / failed | | | 150 / 0 both |
| avg latency (mean of 3 runs) | | | on ≤ off + 5 ms |
| max latency | | | on ≤ off + 20 ms |
| Δ `client attempts` − 150 (extra attempts) | | | equal (both 0 expected) |
| Δ `radio tx` − Δ `hb tx` (data frames incl. retransmissions) | | | within 5 % |
| Δ `radio … failed` | | | within noise (± 2) |
| Σ Δ `dedup replays` on A, B, C | | | equal |

What to expect: no measurable difference. A 5 ms gap holds back at most one or two queued frames; the Wi-Fi hardware still receives and ACKs. If run 2 fails:
* raise `DHT11_READ_INTERVAL_MS` (e.g. 5000) and re-run, or
* keep D's bit clear for the demo (temperature simulated, flagged).

---

## Proposed changes not applied (need approval)

| change | file | when |
|---|---|---|
| Paste the real MACs | `src/platform/transport.cpp:24-38` | if step 0 shows a difference |
| `board_build.flash_mode = dio` | `platformio.ini` `[env]` | only if a SuperMini boot-loops after flashing |
| I2C on GPIO6/7 | `config.h:166-167`, `main.cpp:515` | only if 8/9 misbehave |
| `RADIO_TX_POWER_QDBM` in config | `config.h`, `transport.cpp:224` | if range needs tuning |
| Set `REAL_SENSOR_NODES` bits | `config.h:151` | per module, after its check (4.6) |
| LM393 AO peak-to-peak | `sensor_sources`, `sensors.cpp` | only if the module has AO (4.2) |
