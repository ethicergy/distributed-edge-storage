# edge-kv: notes for Claude Code sessions

Hardware bring-up of the 4-node ESP32-C3 cluster is in progress on branch `hw-bringup`.

**First, read the "RESUME HERE" section at the top of `docs/BRINGUP_LOG.md`.** It holds the
state of every board, what is committed but not flashed, open problems, and the next steps in
order. Keep it current; log every step there (problems §2, decisions §3) with the laptop's
timestamps, and put serial evidence in `docs/test-logs/`.

## Standing rules

- Serial ports: `/dev/serial/by-id/...` only, never `ttyACMx`. After opening a port or a
  replug, send an empty line and wait ~1.2 s. Never `cat`/`echo` to a tty.
- Physical actions (wiring, plugging, buttons) are the owner's (NS): say exactly what to do
  and wait. NS messages at the moment of each action; timestamps come from the laptop clock.
- Wire by silkscreen, never by position. Boards ≥ 20 cm apart, antenna ends clear.
- Record `storage errors` and aborted compactions before and after every step.
- Firmware fixes are test-first (`make check`). Ask before any change NS has not approved.
- `scripts/measure.py` waits after opening ports and deletes its test keys.
- `format yes` on every new EEPROM module. The Pi stays off until NS says otherwise.
- PlatformIO: `.venv/bin/pio`. Commit and push at the points NS names.
