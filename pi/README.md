# edgekv — Raspberry Pi client

Talks to the edge-kv cluster through the bridge node (node A) over the
Pi's UART. Python 3.8+, one dependency (pyserial).

## Set up the Pi

1. Wire: Pi pin 8 (GPIO14 TXD) → node A GPIO20; Pi pin 10 (GPIO15 RXD)
   ← node A GPIO21; Pi GND ↔ node A GND.
2. `sudo raspi-config` → Interface Options → Serial Port: login shell
   over serial **No**, serial hardware **Yes**. Reboot. The port is
   `/dev/serial0`.
3. Install:

   ```bash
   cd pi
   python3 -m pip install --user pyserial    # or: sudo apt install python3-serial
   ```

   (`pip install .` also installs an `edgekv` command.)

## Use

```bash
python3 -m edgekv ping                    # bridge is node A, round trip ...
python3 -m edgekv set greeting hello world
python3 -m edgekv get greeting
python3 -m edgekv del greeting
python3 -m edgekv sensors                 # latest reading of every node
python3 -m edgekv history B               # last 4 readings of node B
python3 -m edgekv watch --interval 30 --csv readings.csv
```

`--port` defaults to `$EDGEKV_PORT` or `/dev/serial0`.

`--raw-log FILE` appends every byte the frame decoder throws away to
FILE (timestamp, reason, text and hex): node A's ROM boot text after a
reset, noise, frames with a bad CRC (wrong baud rate), or a login
prompt if the Pi's serial console is still on.

```bash
python3 -m edgekv --raw-log uart-raw.log watch --interval 5
```

Readings: A `temp` (DHT11), B `motion` (AM312 PIR: events per window,
"last motion N s ago", `settling` after power-on), C `sound` (LM393:
events per window, % of the window active), D `light` (BH1750). The
CSV of `watch --csv` has the columns `read_at, node, kind, value, unit,
sequence, sampled_at, simulated, last_motion_s, active_pct, settling`
(the last three are empty where they do not apply; `last_motion_s` is
counted back from `sampled_at`). Start a new CSV file when upgrading
from the 8-column format.

Exit codes: 0 success, 1 key not found, 2 the cluster refused the
operation, 3 no answer from the bridge.

`OK (under-replicated)` after a write means the value is stored on its
primary only (its replica was unreachable); the cluster copies it to
the replica automatically.

Sample times are converted from the cluster's HLC to the Pi's wall
clock using the bridge's clock in each answer.

## Library

```python
from edgekv import EdgeKV

with EdgeKV("/dev/serial0") as kv:
    kv.set(b"setpoint", b"21.5")
    print(kv.get(b"setpoint"))
    for node, reading in kv.sensors().items():
        print(node, reading.value, reading.unit, kv.wall_time(reading.hlc))
```

## Tests

```bash
python3 -m unittest discover -s tests -v
```

The end-to-end test runs against the cluster simulator
(`make sim` in the repository root) and is skipped when it is not built.
