Site Survey Firmware (XIAO nRF54L15)

This firmware performs IEEE 802.15.4 Packet Error Rate (PER) site-survey testing across multiple boards.
It is based on Nordic's radio_test sample and adds peer discovery, coordinated test runs, and machine-readable UART output.

radio_test sample:
https://github.com/nrfconnect/sdk-nrf/tree/main/samples/peripheral/radio_test

## Objective

Use PER data to evaluate link quality and deployment feasibility.

Typical outcomes:
- Pick the best edge node placement
- Decide whether an intermediary relay/link node is needed
- Compare reliability across candidate installation points

## Node Roles

All boards use the same firmware image and participate in the same PER workflow.

### Aggregator / Coordinator
- One node is connected to the logger/PC.
- It discovers nodes, shares the discovered-node list, coordinates broadcaster rotation, and logs all PER results.
- It also participates in PER as a normal node.
- The aggregator is always the first broadcaster in a `proto_test_run` cycle.

### Broadcaster
- Exactly one node is the broadcaster at a time.
- The broadcaster sends `TEST_START` control messages to each receiver, transmits the PER packet burst, sends `TEST_END`, then requests PER results from each receiver.
- When the broadcaster is not active, it behaves as a receiver.

### Receiver
- Any node that is not the current broadcaster is a receiver.
- Receivers only count `TEST_DATA` packets during an explicitly active test window for the current broadcaster.
- After `TEST_END`, receivers respond to PER requests with the number of packets received.

### Node Type / Aggregator Flag
- `node_aggregator on|off` still marks the logger-connected node.
- `node_type` remains available as persisted metadata, but the coordinator-managed PER sweep treats discovered nodes as identical test participants.

## Quick Start

Use one PC-connected board as controller/logger. Flash the same firmware to all boards.

1. Configure **other boards first** (before coordinator starts discovery)

No configuration needed — all boards boot with factory defaults.

> Factory defaults: data_rate `ieee802154_250Kbit`, channel `17`, node_aggregator `off`, log_mode `minimal`, output_power `pos8dBm`.

2. Configure controller/logger board

```bash
node_mode coordinator
node_aggregator on
```

> Sets this board as the coordinator/gatherer. Factory defaults for other settings.

3. Run discovery from the coordinator

```bash
discover_list_clear
proto_send_discover 400
discover_status
```
start_channel 15
end_channel 15

4. *(Optional)* Change channel on all nodes simultaneously

If you need to move all nodes to a different channel after discovery, use `proto_set_channel`. It sends the new channel to every discovered peer, waits for each to ACK and retune, then changes the coordinator's own channel last.

```bash
proto_set_channel 15
```

> The coordinator only switches channels after all peers confirm. If any peer fails to ACK, the coordinator stays on the original channel.

5. Run full PER flow

```bash
proto_test_run 100 200 200
```

This runs the full coordinator-managed sweep:
- coordinator discovers peers and builds the discovered-node list
- coordinator shares that list to all discovered nodes
- coordinator runs the first broadcaster cycle locally
- each remaining discovered node is then told to become broadcaster in turn
- all PER results are reported back to the aggregator and emitted as `PER_REPORT`

## Required Radio Settings

Protocol commands require IEEE 802.15.4 mode.

```bash
data_rate ieee802154_250Kbit
start_channel 15
```

Valid IEEE channels: 11-26.

## Logging Modes

Use `log_mode <minimal|verbose>` to control output:

- **minimal** (default, recommended): Shows ACK_START, ACK_END, and PER_REPORT. Clean, parseable output for scripted test runs.
- **verbose**: Shows all protocol diagnostics for development/debugging. Use when troubleshooting discovery or test issues.

Example:
```bash
log_mode minimal   # Production - only see PER_REPORT results
log_mode verbose   # Debugging - see all protocol details
```

## Core Commands

| Command | Purpose |
|---|---|
| node_mode <unassigned|coordinator|receiver|test_tx> | Set runtime mode |
| node_type <x|y> | Persist optional node metadata |
| node_aggregator <on|off> | Mark node as data aggregator |
| node_status | Show mode/signatures/type/aggregator |
| discover_list_clear | Clear local discovered list and request discovered peers return to unassigned |
| proto_send_discover [wait_ms] | Discover peers |
| discover_status | Print discovered peers including type + aggregator metadata |
| proto_set_channel <channel> [wait_ms] [retry_ms] | Change channel on all discovered peers, then coordinator (11-26) |
| proto_test_run [packets] [wait_ms] [retry_ms] | Full coordinator-managed broadcaster sweep |
| proto_round_robin_run [packets] [wait_ms] [retry_ms] | Remote broadcaster sweep for discovered peers |
| cancel | Stop active wait loops |

## Machine-Readable UART Output

Your host parser should consume:
- ACK_START,<command>
- ACK_END,<command>,OK|ERR
- DISCOVER_STATUS,index,signature,node_type,is_aggregator
- PER_REPORT,<tx_serial>,<rx_serial>,<total_sent>,<total_received>

During debug builds or when verbose protocol diagnostics are enabled, additional coordinator lifecycle lines may appear, such as `RR_DIAG,...` and `RTW_DIAG,...`.

## Notes

- Boards boot in IEEE 802.15.4 mode on channel 15.
- After reset/power cycle, nodes start in unassigned mode and should be rediscovered.
- node_type and node_aggregator are persisted in non-volatile settings.
- `proto_test_run` shares the discovered-node list to all discovered nodes before rotating broadcasters.
- Control-plane steps use per-node ACK/retry handling. `TEST_DATA` packets remain one-to-many and are intentionally unacked.
- Receivers only count packets inside the current broadcaster's active test window, bounded by explicit start/end commands.
- **Direct communication only**: No relay forwarding. All nodes must be within RF range of each other or directly reachable from the coordinator. If a node has weak bidirectional link (e.g., -100+ dBm), increase TX power (`output_power`) or move it closer.
- `TEST_DATA` remains direct broadcaster-to-receivers traffic (no relay), matching PER measurement behavior.
