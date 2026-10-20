# Results: simulator vs hardware

Every number comes from `scripts/measure.py`, which drives the node consoles: the four boards' USB consoles, or `build/host/edge_sim`. Both columns are measured the same way. The simulator column was filled on 2026-10-17. The hardware column is filled during bring-up (BRINGUP_LOG M7, M11, M12, M14).

The "why they differ" column starts with **what we expect and why**. Replace it with what you actually saw once the hardware number is in.

## How to reproduce

```bash
make sim
scripts/measure.py --sim --sections latency,failover,loss --fill docs/RESULTS.md --column simulator

# boards (normal images): latency + failover; you are asked to unplug/replug node C
scripts/measure.py --ports A=$PA,B=$PB,C=$PC,D=$PD --sections latency,failover \
    --log docs/test-logs/measure-hw.log --fill docs/RESULTS.md --column hardware
# boards with the -fi images: loss
scripts/measure.py --ports A=$PA,B=$PB,C=$PC,D=$PD --sections loss \
    --log docs/test-logs/measure-hw-loss.log --fill docs/RESULTS.md --column hardware
# after the overnight soak (counters since boot): sensors + wear
scripts/measure.py --ports A=$PA,B=$PB,C=$PC,D=$PD --sections sensors,wear \
    --log docs/test-logs/measure-hw-soak.log --fill docs/RESULTS.md --column hardware
```

`$PA` … are the `/dev/serial/by-id/…` paths of the boards. `--json FILE` keeps the raw numbers. Each run also leaves a console transcript (`--log`) as evidence.

Procedure details:
* **Latency:** 50 console SETs then 50 GETs per node on 8 keys, sensor samplers off. Each op's latency is the one the console prints: from the console submitting it to the reply, including retries.
* **Failover:** victim C. A probe key with primary C is read from A in a loop from the moment C loses power. Then 10 keys with primary C are written from B while C is down, C is powered on, and the 10 keys are read from C.
* **Loss:** `fault loss <p>` on all four nodes drops p % of every frame each node sends, so that share is lost on each link. Then `bench 50` from A and from B.

## 1. Latency (no loss)

| id | metric | simulator | hardware | why they differ |
|---|---|---|---|---|
| `set_bridge` | SET from node A (Pi bridge), min / avg / p95 / max | 6 / 11 / 14 / 17 ms (n=50) | | Expected: hardware higher. The simulator's EEPROM writes are instant, but a real AT24C256 page write takes ≤ 5 ms. A SET writes on the primary and the replica (1–2 pages each), and ESP-NOW air time + MAC ACK adds about 1 ms per hop. Expect roughly +10–25 ms. |
| `set_other` | SET from B, C, D | 3 / 42 / 15 / 2428 ms (n=150) | | Avg and max come from 2 ops that hit **dedup table full** on D (section 5): 64 entries, reusable after 3.25 s, so a node accepts about 20 writes/s in sustained bursts. The client backed off and the 4th attempt succeeded. p95 (15 ms) shows the normal case. Expect the same effect on boards, since it is a capacity limit, not a timing artefact. |
| `get_bridge` | GET from A | 3 / 6 / 8 / 9 ms (n=50) | | GETs are not replicated and only read EEPROM (~1 ms per 64 B at 400 kHz). Expect hardware a few ms higher than simulator. |
| `get_other` | GET from B, C, D | 1 / 5 / 9 / 9 ms (n=150) | | Same. 1 ms is the local fast path (the console node is the primary, so no radio). |
| `latency_retries` | ops needing more than 1 attempt | 2 of 400 ops needed >1 attempt | | Simulator: both are the dedup-table case above. Hardware adds real radio losses (check `stats` → `esp-now tx … failed`). |

## 2. Failure and recovery (victim C)

| id | metric | simulator | hardware | why they differ |
|---|---|---|---|---|
| `failover_detect` | power off → A's view drops C | 1907 ms | | Set by `MEMBERSHIP_TIMEOUT_MS` = 2000 minus the age of C's last heartbeat (sent every 500 ms), so 1.5–2.0 s on both. Hardware measures from the USB port vanishing, which lags the power cut by up to ~0.1 s (more under WSL/usbip). |
| `failover_client` | power off → first GET of a C-owned key served by another node | 3407 ms (served by D) | | The GET in flight when C died waits out its attempt deadline (`CLIENT_ATTEMPT_DEADLINE_MS` = 3 s), then the next attempt goes to the new owner D. Expect about 3.4 s on both. |
| `outage_writes` | writes to C-owned keys while C is down | 10 OK (of 10) | | Writes go to the next node on the ring, which replicates to its own successor, so `OK`, not under-replicated. Expect the same. |
| `catchup` | C: power-on → ACTIVE | 5500 ms (power-on -> ACTIVE) | | Simulator: `BOOT_LISTEN_MS` 2.5 s + `CATCHUP_DURATION_MS` 3 s. The board measures from boot (uptime), so it adds the 1.5 s console delay in `setup()` and EEPROM recovery: expect about 7–7.5 s. |
| `catchup_view` | C: power-on → A's view includes C again | 5501 ms | | C heartbeats only once ACTIVE, so this matches `catchup`. On boards it is measured from the port reappearing (after USB enumeration), so it reads lower than `catchup`. |
| `keys_repaired` | keys written while C was down, readable from C afterwards | 10/10 written while down served by C afterwards; resync applied 10, repair log lines 10 | | Anti-entropy push during CATCHUP (2 version-table rounds). Expect 10/10 on both. More `resync applied` than 10 on boards means sensor keys were repaired too. |

## 2b. Power cycle, one node (manual check, BRINGUP step 2)

Not driven by `measure.py`: read from `status` before and after unplugging node A (`docs/test-logs/step2-node-a.log`).

| id | metric | simulator | hardware | why they differ |
|---|---|---|---|---|
| `hlc_reboot` | HLC after a power cycle continues from the last persisted version instead of restarting at 0 | `test/hlc_test.cpp` `test_reboot_anchor`: timer 100 ms, persisted 1000 ms → clock reads 1000 | **pass.** Before: uptime 37 s, `hlc 37466.0`. Unplug ~07:01, replug. After: uptime 73 s, `hlc 466675.0`, `reset reason: power-on` | Same rule on both (`node_runtime.cpp:271-275`, `hlc.cpp:43-47`): the clock starts at or above the newest persisted version. After the cycle the timer had restarted (73 s), but the HLC was 466.7 s, i.e. the ~393 s the node ran before the unplug plus the time since boot. Without this, new writes after a reboot would carry older timestamps than the ones on disk and lose to them. Sensor `seq` also continued (1 → 16). |
| `persist_reboot` | key written before a power cycle readable after it | storage crash tests (`test/storage_manager_test.cpp`) | **pass.** `set x 1` → unplug/replug → `storage recovered: 6 live`, `x = "1"`, storage errors 0 | Also rules out WP high on this module (writes would silently vanish). |
| `h11_format_refill` | H11-style: `format yes` on B, keys refilled by anti-entropy | `test/cluster_test.cpp` and the simulator (`format yes` on a node, `keys_repaired` above) | **pass.** Before: 39 keys on B = 32 owned (3 tombstones) + 7 stand-in copies; every key also on its other owner. After: **32/32 owned keys back at the same version, 0 missing, 0 extra**, stand-in copies gone. `format yes` 13:43:50 → `storage formatted` + `BOOTING` 2.07 s → `CATCHUP` 2.52 s → last repair 3.42 s → `ACTIVE` 5.55 s; first `dump` after ACTIVE matched. 35 repairs from A 10, C 13, D 12 | Erasing the EEPROM takes ~2 s, the refill itself under 1 s (all pushes in the first CATCHUP round); ACTIVE is then fixed by `CATCHUP_DURATION_MS` (3 s). Samplers were off so versions stayed fixed. The refilled set includes foreign keys from B's module (BRINGUP_LOG problem 7). Log `docs/test-logs/H11-node-b.log` |

## 3. Loss tolerance (`fault loss`, `bench 50`; boards need the `-fi` images)

| id | metric | simulator | hardware | why they differ |
|---|---|---|---|---|
| `loss10_A` | 10 % loss, bench from A: ok/failed, min/avg/max | 50 ok, 0 failed; latency 6 / 444 / 3406 ms (min/avg/max) | | Each lost request or reply costs a retransmission timeout (600 ms, then 1200 ms). Boards add their own real losses on top of the injected ones. Expect avg and max a bit higher, but still 0 failed. |
| `loss10_B` | 10 % loss, bench from B | 50 ok, 0 failed; latency 8 / 256 / 1814 ms (min/avg/max) | | As above. |
| `loss10_retries` | extra client attempts / duplicates answered from the dedup cache | 1 extra client attempts, 30 duplicate requests answered from cache | | Duplicates answered from the cache are retransmissions whose first copy had already executed: exactly-once at work. Extra attempts are whole-attempt timeouts (rare at 10 %). |
| `loss20_A` | 20 % loss, bench from A | 50 ok, 0 failed; latency 7 / 731 / 5211 ms (min/avg/max) | | Matches the earlier `loss 0.2` runs (avg ~0.6–1 s). |
| `loss20_B` | 20 % loss, bench from B | 50 ok, 0 failed; latency 6 / 821 / 3414 ms (min/avg/max) | | |
| `loss20_retries` | extra client attempts / duplicates answered from the dedup cache | 8 extra client attempts, 40 duplicate requests answered from cache | | |

## 4. Sensors (after the overnight soak)

**Informal data point, Tue 20 13:32 (not a soak):** the boards did not run unattended overnight. All four reported `reset reason: power-on` with uptimes of 4-10 min (A 572 s, B 425 s, C 240 s, D 486 s): they were powered up again around 13:22-13:28 when re-attached. Counters since that boot: storage errors 0 on all four; hottest EEPROM page 7 (A, page 262), 8 (B, page 8), 5 (C, page 363), 11 (D, page 65) writes; compactions A 1, B 1, C 0, D 2; 0 failed ESP-NOW sends on any link; heap and stack headroom unchanged from Mon (stack 13.2-13.5 KB free). Log: `docs/test-logs/tue20-1332-state.log`. The overnight soak (H13, M10) is still to do.

The simulator column is the 2-hour soak of `cluster_test H13`: all four nodes, 30 s period, a reboot every 20 min.

| id | metric | simulator | hardware | why they differ |
|---|---|---|---|---|
| `sensor_A` | A (BH1750 light): samples, writes committed, read failures | H13 (all nodes, 2 h, 5 reboots): 1918 writes ok, 0 failed, 0 storage errors | | Simulated sources never fail to read. A BH1750 read failure is an I2C error (`read failures`). After 3 in a row the simulated source stands in, and its readings are flagged `simulated`. |
| `sensor_B` | B (AM312 motion) | as above | | PIR reads never fail. The first `PIR_SETTLE_MS` of samples are `settling`. |
| `sensor_C` | C (LM393 sound) | as above | | Reads never fail. Event counts depend on the knob setting. |
| `sensor_D` | D (DHT11 temperature) | as above | | DHT11 checksum or timeout failures are counted in `read failures`. The last good value is kept, and after 10 s without a good read it is flagged `STALE`. |

## 5. EEPROM wear (after the overnight soak)

| id | metric | simulator | hardware | why they differ |
|---|---|---|---|---|
| `wear_A` | hottest page writes per hour → years to the 1M-cycle rating | H13: hottest page 27 writes in 2.0 h → 8.5 years (all nodes, 30 s period) | | Same counting on both: the simulator's fake EEPROM and the firmware driver both count page write cycles. Differences come from the write rate: sample period, real event counts, user writes during the soak. A short run overstates wear because formatting and catch-up writes dominate. Use at least a few hours. |
| `wear_B` | B | as above | | |
| `wear_C` | C | as above | | |
| `wear_D` | D | as above | | |

## 6. Findings worth stating in the report

1. **Write rate per node is bounded by the dedup table.**
   * Each node keeps the last 64 mutation ids (`MAX_DEDUP_ENTRIES`, `config.h:93`). A completed entry can be reused after `DEDUP_EVICT_AFTER_MS` = 3.25 s (`config.h:82`).
   * So a node can take about 64 / 3.25 s ≈ 20 mutations per second, counting both client SETs and replications. Beyond that it answers `STORAGE_ERROR` and the client backs off.
   * The measurement hit this with sequential console SETs (2 of 400 ops).
   * The trade-off is RAM (about 85 B per entry) against burst rate. Retention must stay above the retransmission deadline, or exactly-once breaks; the `static_assert`s in `config.h` enforce that.
2. **Failover costs a client about 3.4 s** (one attempt deadline) **even though membership notices in under 2 s.** A shorter `CLIENT_ATTEMPT_DEADLINE_MS` would cut this but raise false timeouts under loss. The `static_assert`s keep the timeouts in a safe order.
3. **Catch-up is constant** (2.5 s + 3 s) and independent of how many keys were missed, because all repairs are pushed during the CATCHUP window.

## Notes for the hardware run

* Run the latency section first, on fresh boards. The failover section reboots C.
* The loss section needs the `-fi` images on all four boards. Reflash the normal images afterwards.
* The sensors and wear sections read counters since boot. Run them at the end of the soak, before rebooting anything.
* Record each run in `docs/BRINGUP_LOG.md` (M7, M11, M12, M14) with the transcript file name.
