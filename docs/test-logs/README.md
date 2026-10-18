# Test logs

Evidence for the test matrix in docs/HARDWARE_TEST_PLAN.md.

| file | kind |
|---|---|
| `host-tests.log` | host run of `make check` + the cluster scenarios (simulation, not hardware) |
| `step1-node-<x>.log` | BRINGUP step 1 (no EEPROM): boot banner and FATAL of node x |
| `H<n>-node-<x>.log` | serial log of node x during hardware test H<n> (docs/HARDWARE_TEST_PLAN.md) |
| `H14-pi.log` | Raspberry Pi terminal output for H14 |

A hardware test is passed only when its logs are here. Host results do
not replace them: the simulation has no EEPROM timing, no radio
interference and no USB/UART quirks.
