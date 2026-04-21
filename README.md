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

### Node Mode (runtime behavior)
Use node_mode for runtime control:
- unassigned
- coordinator
- receiver
- test_tx

### Node Type (site-survey role)
Use node_type for survey policy:
- x: link-capable node, can run PER against x and y peers
- y: sensor/data node, can run PER only against x peers

### Aggregator Flag
Use node_aggregator to mark the board connected to your logger/PC:
- on: this board is the data aggregator endpoint
- off: normal node

## Quick Start

Use one PC-connected board as controller/logger. Flash the same firmware to all boards.

1. Configure controller/logger board

```bash
node_mode coordinator
node_type x
node_aggregator on
log_mode minimal
```

2. Configure other boards (examples)

```bash
# Link-capable board
node_type x
node_aggregator off

# Sensor/data board
node_type y
node_aggregator off
```

3. Build a fresh discovery set

```bash
discover_list_clear
proto_send_discover 500
discover_status
```

4. Run full PER flow

```bash
proto_test_run 100 200 200
```

This runs local TX/PER collection plus delegated round-robin tests and emits PER_REPORT lines.

## Required Radio Settings

Protocol commands require IEEE 802.15.4 mode.

```bash
data_rate ieee802154_250Kbit
start_channel 15
```

Valid IEEE channels: 11-26.

## Core Commands

| Command | Purpose |
|---|---|
| node_mode <unassigned|coordinator|receiver|test_tx> | Set runtime mode |
| node_type <x|y> | Set site-survey node type |
| node_aggregator <on|off> | Mark node as data aggregator |
| node_status | Show mode/signatures/type/aggregator |
| discover_list_clear | Clear local discovered list and request discovered peers return to unassigned |
| proto_send_discover [wait_ms] | Discover peers |
| discover_status | Print discovered peers including type + aggregator metadata |
| proto_test_run [packets] [wait_ms] [retry_ms] | Full local + delegated PER run |
| proto_round_robin_run [packets] [wait_ms] [retry_ms] | Delegated-only run |
| cancel | Stop active wait loops |

## Machine-Readable UART Output

Your host parser should consume:
- ACK_START,<command>
- ACK_END,<command>,OK|ERR
- DISCOVER_STATUS,index,signature,node_type,is_aggregator
- PER_REPORT,<tx_serial>,<rx_serial>,<total_sent>,<total_received>

## Notes

- Boards boot in IEEE 802.15.4 mode on channel 15.
- After reset/power cycle, nodes start in unassigned mode and should be rediscovered.
- node_type and node_aggregator are persisted in non-volatile settings.
- Y->Y PER collection is intentionally blocked by policy; y nodes only target x peers.
