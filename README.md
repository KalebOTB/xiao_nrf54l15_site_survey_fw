# Site Survey Firmware

This repository contains a Zephyr-based IEEE 802.15.4 site-survey firmware derived from Nordic's `radio_test` sample.
It adds peer discovery, coordinated multi-node PER testing, machine-readable UART output, persisted node state, and delegated broadcaster rounds for whole-network link characterization.

Upstream sample reference:
https://github.com/nrfconnect/sdk-nrf/tree/main/samples/peripheral/radio_test

## Quick Start

This section is the shortest path from a fresh checkout to a usable PER run.

### 1. Prerequisites

You need:
- nRF Connect SDK / Zephyr with `west`
- a supported board and debug probe
- a serial terminal connected to the shell transport used by the image

This project uses sysbuild.

### 2. Build

Example build for an nRF54L15 DK application core target:

```bash
west build -p always -b nrf54l15dk/nrf54l15/cpuapp --sysbuild .
```

### 3. Flash

```bash
west flash
```

Flash the same firmware image to every node in the test set.

### 4. Bring up the coordinator

On the node connected to your PC/logger:

```bash
node_mode coordinator
node_aggregator on
log_mode verbose
```

Useful factory defaults:
- `data_rate ieee802154_250Kbit`
- channel `17`
- `node_aggregator off`
- `log_mode minimal`
- `output_power pos8dBm`

### 5. Discover the other nodes

```bash
discover_list_clear
proto_send_discover 400
discover_status
```

### 6. Optionally move the whole group to a new channel

```bash
proto_set_channel 15
```

The coordinator changes channel last. If any discovered peer fails to acknowledge the change, the coordinator stays on the original channel.

### 7. Run a full PER sweep

```bash
proto_test_run 10 200 200 64
```

This means:
- `10`: packets per broadcaster round
- `200`: `wait_ms`
- `200`: `retry_ms`
- `64`: total `TEST_DATA` payload length in bytes

`payload_len` must be in the range `20..127`. `20` is the protocol header only. Bytes beyond the header are randomized before transmit.

## Project Overview

### Objective

Use packet error rate data to answer deployment questions such as:
- whether two nodes have sufficient direct link margin
- which placement gives the most reliable edge connectivity
- whether an additional intermediate node is needed
- how payload size and channel affect reliability

### Runtime Roles

All nodes run the same image. Roles are runtime state, not separate binaries.

#### Coordinator

The coordinator:
- discovers peers
- shares the discovered-node list to all participating nodes
- initiates local and delegated test rounds
- collects and prints machine-readable results
- also participates as a receiver in the PER matrix

#### Broadcaster

The active broadcaster:
- opens a test window on receivers with `TEST_START`
- sends the `TEST_DATA` burst
- closes the window with `TEST_END`
- requests each receiver's packet count with `PER_REQ`

#### Receiver

A receiver:
- counts `TEST_DATA` only while the test session is active for the current broadcaster
- returns its counted packet total in `PER_RSP`

### Test Modes

The repo supports three main PER workflows:

#### `proto_tx_run`

The local node broadcasts once and then directly collects PER from discovered peers.

#### `proto_round_robin_run`

Each discovered peer becomes a delegated broadcaster in turn and reports its results back to the coordinator.

#### `proto_test_run`

This is the end-to-end workflow:
- coordinator broadcasts first
- coordinator collects direct PER for its own round
- coordinator then asks each discovered peer to run a delegated round

## How PER Reporting Works

### Local coordinator round

For the coordinator's own broadcast round:
- coordinator sends `TEST_START`
- coordinator sends `TEST_DATA`
- coordinator sends `TEST_END`
- coordinator sends `PER_REQ` to each discovered peer
- each peer responds with `PER_RSP`
- coordinator prints `PER_REPORT`

### Delegated round

For a delegated broadcaster:
- coordinator sends `REMOTE_TEST_REQ`
- delegate acknowledges and runs its own `TEST_START` / `TEST_DATA` / `TEST_END`
- delegate sends `PER_REQ` to the other nodes
- receivers answer with `PER_RSP`
- delegate reports each result back to the coordinator with `REMOTE_TEST_REPORT`
- coordinator prints `PER_REPORT`

### Sentinel values

`PER_REPORT` uses the same output shape for real and synthetic results:

```text
PER_REPORT,<tx_serial>,<rx_serial>,<total_sent>,<total_received>,<payload_len>,<channel>
```

Meaning of special `total_received` values:
- `-1`: the test ran, but that receiver never produced a usable PER response
- `-2`: the delegated round result is unknown because the delegate failed to communicate the round back to the coordinator

In addition, the firmware emits:

```text
PER_REPORT_TIMEOUT,<tx_serial>,<rx_serial>,<total_sent>,<payload_len>,<channel>
```

### `wait_ms` and `retry_ms`

`wait_ms` is the receive window after a request is sent.

Examples:
- how long the broadcaster waits for `PER_RSP`
- how long control-plane ACK logic waits for a response
- how long the system waits after `TEST_DATA` before requesting PER

`retry_ms` is the delay before trying again after a miss.

Practical rule:
- lower values make testing faster
- higher values improve tolerance for weak or slow links

## Build and Configuration

### Repository Layout

Top-level files and directories:

| Path | Purpose |
|---|---|
| `CMakeLists.txt` | Zephyr application entry for the app image |
| `Kconfig` | Project Kconfig options |
| `prj.conf` | Main app configuration |
| `prj_usb.conf` | USB shell variant configuration |
| `sysbuild.conf` | Sysbuild-level configuration |
| `sysbuild.cmake` | Sysbuild image composition logic |
| `boards/` | Board-specific `.conf` and overlay files |
| `src/main.c` | Board bring-up, clock/UI init, node startup |
| `src/radio_cmd.c` | Shell commands, protocol orchestration, coordinator logic |
| `src/radio_test.c` | Radio and protocol frame handling |
| `src/radio_test.h` | Protocol definitions and radio interfaces |
| `src/radio_node.h` | Node-mode interface declarations |
| `src/local_config.h` | Compile-time verbose logging toggles |

### Config Highlights

From `prj.conf`:
- shell enabled
- flash settings enabled through NVS / settings
- entropy enabled for randomized payload fill
- shell stack enlarged to avoid overflow

From `prj_usb.conf`:
- enables `CONFIG_RADIO_TEST_USB` for nRF5340 USB shell transport

From `sysbuild.cmake`:
- on nRF5340 CPUNET builds, an app-core `remote_shell` image is included automatically

### Logging Controls

Runtime logging mode:

```bash
log_mode minimal
log_mode verbose
```

Meaning:
- `minimal`: intended for host-side parsers, keeps output mostly to ACK and PER lines
- `verbose`: enables coordination diagnostics such as `CTRL_DIAG`, `RR_DIAG`, `RTW_DIAG`, `BOUNDARY_DIAG`, and `PER_DIAG`

Compile-time debug toggles live in `src/local_config.h`.

## Operating Guide

### Required Radio Mode

Protocol commands require IEEE 802.15.4 mode.

```bash
data_rate ieee802154_250Kbit
start_channel 15
```

Valid IEEE channels are `11..26`.

### Common Workflow

#### Clean slate

```bash
discover_list_clear
```

Use this before a new survey session if the peer list may be stale.

#### Discovery

```bash
proto_send_discover 400
discover_status
```

#### Direct coordinator-only round

```bash
proto_tx_run 100 200 200 64
```

#### Full coordinated round-robin survey

```bash
proto_test_run 100 200 200 64
```

#### One-off direct PER request

```bash
proto_send_per_req 0xe044c610 100 200
```

This is useful when you want the coordinator to directly query one node for its received count.

### Channel Changes

Use `proto_set_channel` instead of manually reconfiguring discovered peers one by one:

```bash
proto_set_channel 15 200 200
```

The command emits `ACK_START` and `ACK_END` like other coordinated control operations.

## Command Reference

### Site-survey commands

| Command | Purpose |
|---|---|
| `node_mode <unassigned|coordinator|receiver|test_tx>` | Set the node's runtime role |
| `node_type <x|y>` | Persist optional node metadata |
| `node_aggregator <on|off>` | Mark the logger-connected node |
| `node_status` | Print persisted node state and signatures |
| `node_factory_reset` | Clear persisted node assignment/state |
| `log_mode <minimal|verbose>` | Set runtime logging mode |
| `proto_send_discover [wait_ms]` | Broadcast discovery and gather responses |
| `discover_status` | Print discovered peer list |
| `discover_list_clear` | Clear local discovered state and request peers return to unassigned |
| `proto_set_channel <channel> [wait_ms] [retry_ms]` | Change channel across the discovered set |
| `proto_send_test_data [packets] [payload_len]` | Send a direct `TEST_DATA` burst |
| `proto_send_per_req <dst_sig> <expected_packets> [wait_ms]` | Directly query one node for PER |
| `proto_collect_per <expected_packets> [wait_ms] [retry_ms]` | Collect PER from all discovered peers |
| `proto_tx_run [packets] [per_wait_ms] [retry_ms] [payload_len]` | Local broadcast + local PER collection |
| `proto_round_robin_run [packets] [wait_ms] [retry_ms] [payload_len]` | Delegated broadcaster sweep |
| `proto_test_run [packets] [wait_ms] [retry_ms] [payload_len]` | Full local + delegated PER workflow |
| `proto_status` | Print protocol counters and discovered peers |
| `proto_reset` | Reset protocol counters and peer state |
| `cancel` | Cancel active sweeps, waits, or loops |

### Underlying radio-test commands

The original Nordic sample shell surface is still present for low-level radio work, including:
- `data_rate`
- `start_channel`
- `end_channel`
- `output_power`
- `transmit_pattern`
- `start_rx`
- `start_tx_carrier`
- `start_tx_modulated_carrier`
- `start_rx_sweep`
- `start_tx_sweep`
- `print_rx`
- `parameters_print`
- `rssi_monitor`

## Machine-Readable UART Output

Your host parser should consume:
- `ACK_START,<command>`
- `ACK_END,<command>,OK|ERR`
- `DISCOVER_STATUS,index,signature,node_type,is_aggregator`
- `PER_REPORT,<tx_serial>,<rx_serial>,<total_sent>,<total_received>,<payload_len>,<channel>`
- `PER_REPORT_TIMEOUT,<tx_serial>,<rx_serial>,<total_sent>,<payload_len>,<channel>`

Verbose mode can also emit diagnostics such as:
- `CTRL_DIAG,...`
- `RR_DIAG,...`
- `RTW_DIAG,...`
- `BOUNDARY_DIAG,...`
- `PER_DIAG,...`

These diagnostics are useful for debugging but should not be treated as the stable host-parser contract.

## Troubleshooting

### No `PER_REPORT` rows for a link

Check whether the missing row should be:
- `-1`: receiver-specific PER response was missing after a real test attempt
- `-2`: delegate round result is unknown because the delegate never properly communicated the round back to the coordinator

Verbose logs to inspect:
- `CTRL_DIAG,...` for control-plane ACK failures
- `RR_DIAG,req,...` for delegate-start failures
- `RTW_DIAG,per_rsp_timeout,...` for delegate-side receiver misses
- `RTW_DIAG,report,...` for delegate-to-coordinator report delivery issues

### Weak links after moving a node

This firmware measures direct communication, not relay forwarding.
If a moved node can still talk to the coordinator but not to another peer, that peer-pair can still fail during delegated rounds.

Mitigations:
- raise TX power with `output_power`
- reduce `payload_len`
- increase `wait_ms` / `retry_ms`
- move nodes closer or adjust orientation

### Verifying payload length

`payload_len` is the total protocol payload length, not just the random tail.
Current valid bounds are `20..127`.

Example:
- `payload_len=20`: header only
- `payload_len=64`: header plus 44 randomized bytes

## Notes

- All nodes run the same image.
- Node role, metadata, and discovery state are runtime concepts.
- Nodes boot in IEEE 802.15.4 mode on channel `17` unless reconfigured.
- Persisted node metadata is stored through Zephyr settings/NVS.
- `TEST_DATA` remains broadcaster-to-receiver traffic with no relay forwarding.
- Receivers count packets only inside the explicit `TEST_START` / `TEST_END` window for the active broadcaster.
