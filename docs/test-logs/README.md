# Test logs

Evidence for the test matrix in docs/HARDWARE_TEST_PLAN.md.

| file | kind |
|---|---|
| `host-tests.log` | host run of `make check` + the cluster scenarios (simulation, not hardware) |
| `step1-node-<x>.log` | BRINGUP step 1 (no EEPROM): boot banner and FATAL of node x |
| `step2-node-a.log` | BRINGUP step 2 (EEPROM on board 1): status, set/get, stats, then the power-cycle check |
| `step2-node-c.log` | BRINGUP step 2 on node C, with the power-cycle (WP) check |
| `step3-A-C.log` | BRINGUP step 3 with A + C (B deferred): `ring` scan, members, set/get across nodes |
| `step2-node-d.log` | BRINGUP step 2 on node D, with the power-cycle (WP) check |
| `step4-A-C-D.log` | three nodes A + C + D (B deferred): members, `ring`, set/get across nodes, D unplugged, writes, rejoin and repair |
| `tue20-1332-state.log` | Tue 20 13:32: status/stats of all four before anything was touched |
| `step2-node-b.log` | BRINGUP step 2 on node B (after resoldering), with the power cycle |
| `H11-node-b.log` | H11-style: pre-format dumps and ring on every node, `format yes` on B, timed refill |
| `step3-four-nodes.log` | four nodes: members, ring of every key, stand-in copies and their GC, set/get on B-owned keys from each node |
| `H2.log` | H2 with one write per node (B k20, C k4, D k0, A k2) and the owners' dumps |
| `problem7-foreign-keys.log` | deleting the foreign keys from B's module, tombstone GC, push-back after one-at-a-time compaction, simultaneous compaction |
| `wed21-health.log` | Wed 21 06:50: status/stats/members and dumps of all four after the overnight power-off (problem 13: full key index) |
| `wed21-format.log` | Wed 21 06:54: final dumps, `format yes` on all four in parallel, 60 s watch, heartbeat rate per link (problem 14) |
| `sensor-A.log` | M9a: BH1750 on A, after flash, baseline, torch, cover/uncover, EEPROM coexistence bench at 1 s sampling |
| `sensor-D.log` | M9d: DHT11 on D: wiring problems 16/17, boots, continuous capture with breathe, DATA off (STALE), DATA on; D's compaction aborts (problem 15) |
| `dht-radio-off.log` / `dht-radio-on.log` | BRINGUP 4.8 radio check, bit off / on (`-attempt1`: DHT not reading, invalid) |
| `A-capture-wed21.log` | continuous console capture of A during M9d (problem 15) |
| `H<n>-node-<x>.log` | serial log of node x during hardware test H<n> (docs/HARDWARE_TEST_PLAN.md) |
| `H14-pi.log` | Raspberry Pi terminal output for H14 |

A hardware test is passed only when its logs are here. Host results do
not replace them: the simulation has no EEPROM timing, no radio
interference and no USB/UART quirks.
