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
| `H<n>-node-<x>.log` | serial log of node x during hardware test H<n> (docs/HARDWARE_TEST_PLAN.md) |
| `H14-pi.log` | Raspberry Pi terminal output for H14 |

A hardware test is passed only when its logs are here. Host results do
not replace them: the simulation has no EEPROM timing, no radio
interference and no USB/UART quirks.
