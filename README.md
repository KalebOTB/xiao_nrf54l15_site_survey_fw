Site Survey

This firmware is designed for the XIAO nRF54L15.
It is built off of Nordics radio_test sample. A link below provides more information about the radio_test sample and how to use it.

Changes from the original radio_test sample:
- Ported for the XIAO nRF54L15
- Added custom IEEE 802.15.4 protocol with device signatures for PER testing
- Each device has a unique 32-bit signature (auto-generated or manually set)
- Transmitter can discover multiple receivers, send numbered test packets, and collect per-device PER
- Receivers auto-respond to discovery and PER requests (no manual intervention required)
- Enabled 2-byte CRC (16-bit FCS) for IEEE802.15.4

## IEEE 802.15.4 Protocol for Multi-Device Testing

### Protocol Overview

The firmware implements a request/response protocol for coordinating PER tests across multiple devices:

**Commands:**
- **DISCOVER_REQ** (TX broadcast): "Who wants to participate in testing?"
- **DISCOVER_RSP** (RX unicast): "I'm here, my signature is 0xXXXXXXXX"
- **TEST_DATA** (TX broadcast): Numbered packets for PER measurement
- **PER_REQ** (TX unicast): "Send me your received packet count"
- **PER_RSP** (RX unicast): "I received N packets out of expected M"

### Quick Start: Automated One-Command Test

#### Setup (all devices)
```bash
# Enable the right radio mode
data_rate ieee802154_250Kbit
start_channel 15

# Set a unique signature per device
proto_signature 0x12345678   # TX device
# OR
proto_signature 0x87654321   # RX device 1
# OR
proto_signature 0xAABBCCDD   # RX device 2
```

#### Run on TX device only (RX devices listen automatically)
```bash
proto_rx_start     # On all RX devices first (they listen and auto-respond)
proto_tx_run 100 250 80
```

**What this does:**
1. TX broadcasts DISCOVER_REQ → all RX devices reply with their signatures
2. TX waits 250 ms to collect all DISCOVER_RSP messages
3. TX sends 100 TEST_DATA packets to all RX devices
4. TX requests PER from each discovered RX device
5. TX waits 80 ms for PER responses, then prints final statistics

**Output example:**
```
proto role=1 sig=0x12345678 discover_req=1 discover_rsp=2 test_data=0 per_req=2 per_rsp=2 local_test_rx=0 peers=2
peer[0] sig=0x87654321 discover=1 per=1 reported_rx=97
peer[1] sig=0xAABBCCDD discover=1 per=1 reported_rx=99
```

Interpretation:
- TX sent 100 TEST_DATA packets
- Device 0x87654321 received 97 → **PER = 3%**
- Device 0xAABBCCDD received 99 → **PER = 1%**

---

### Manual Step-by-Step Control

If you want to debug intermediate steps or use custom patterns:

**RX Device 1:**
```bash
data_rate ieee802154_250Kbit
start_channel 18
proto_signature 0x87654321
proto_role rx
proto_rx_start     # Passive listening, auto-responds to all requests
```

**RX Device 2:**
```bash
data_rate ieee802154_250Kbit
start_channel 18
proto_signature 0xAABBCCDD
proto_role rx
proto_rx_start
```

**TX Device:**
```bash
data_rate ieee802154_250Kbit
start_channel 18
proto_signature 0x12345678
proto_role tx
proto_tx_run 100 250 80

proto_send_test_data 100
# Send discovery request
proto_send_discover
# RX devices automatically reply after random backoff (15-39 ms)
# TX command now opens a temporary RX window and collects those responses

# Check who responded
proto_status   # Shows discovered peers

# Send test packets (100 packets)
proto_send_test_data 100

# Wait for transmissions to complete
# (device waits ~50ms + packets/4 ms internally)

# Request PER from device 1
proto_send_per_req 0x87654321 100

# Request PER from device 2
proto_send_per_req 0xAABBCCDD 100

# PER request commands automatically open a temporary RX window
# and print whether each targeted receiver responded

# Final statistics
proto_status
```

---

### Command Reference

| Command | Purpose | Example |
|---------|---------|---------|
| `data_rate ieee802154_250Kbit` | Enable 802.15.4 mode (required for protocol) | `data_rate ieee802154_250Kbit` |
| `start_channel <11–26>` | Set IEEE channel (11=2405MHz, 15=2425MHz, 20=2450MHz, 26=2480MHz) | `start_channel 15` |
| `proto_signature <u32>` | Set unique device identifier | `proto_signature 0x12345678` |
| `proto_role <disabled\|tx\|rx>` | Set device mode | `proto_role rx` |
| `proto_tx_run [pkts] [disc_ms] [per_ms]` | **Automated full test** | `proto_tx_run 100 250 80` |
| `proto_rx_start` | Start listening (auto-responds to requests) | `proto_rx_start` |
| `proto_send_discover [wait_ms]` | Broadcast "who's there?" and listen for replies | `proto_send_discover 300` |
| `proto_send_test_data [pkts]` | Send numbered test packets | `proto_send_test_data 100` |
| `proto_send_per_req <sig> <expected> [wait_ms]` | Query specific device's RX count and wait for reply | `proto_send_per_req 0x87654321 100 120` |
| `proto_status` | Print all counters, discovered peers, and PER | `proto_status` |
| `proto_reset` | Clear counters (between test runs) | `proto_reset` |

---

### Understanding PER Output

When you run `proto_status`, you see:

```
proto role=1 sig=0x12345678 discover_req=1 discover_rsp=2 test_data=0 per_req=2 per_rsp=2 local_test_rx=0 peers=2
peer[0] sig=0x87654321 discover=1 per=1 reported_rx=97
peer[1] sig=0xAABBCCDD discover=1 per=1 reported_rx=99
```

**TX device fields:**
- `sig=0x12345678`: This device's signature
- `discover_rsp=2`: Received 2 DISCOVER_RSP messages (found 2 receivers)
- `test_data=0`: TX devices don't receive TEST_DATA (always 0)
- `per_rsp=2`: Received 2 PER_RSP messages
- `reported_rx=97 / 99`: Packet counts from each RX device

**RX device fields (when polled):**
- `discover_req=1`: Received the DISCOVER_REQ
- `reported_rx=97`: Counted 97 packets during the test run

**Calculate PER per peer:**
- TX sent 100 TEST_DATA packets
- Peer 0x87654321 reported receiving 97 → **Packet Error Rate = (100-97)/100 = 3%**
- Peer 0xAABBCCDD reported receiving 99 → **Packet Error Rate = (100-99)/100 = 1%**

---

### Typical Test Procedure

1. **Ensure all devices are on the same channel:**
   ```bash
   data_rate ieee802154_250Kbit
   start_channel 15
   ```

2. **Assign unique signatures:**
   ```bash
   # On TX: proto_signature 0x11111111
   # On RX1: proto_signature 0x22222222
   # On RX2: proto_signature 0x33333333
   ```

3. **Start all RX devices listening:**
   ```bash
   proto_role rx
   proto_rx_start
   ```

4. **Run test from TX:**
   ```bash
   proto_tx_run 1000 300 150
   # Discovers in 300ms, sends 1000 packets, waits 150ms for PER responses
   ```

5. **Inspect results:**
   ```bash
   proto_status
   ```

6. **Between test runs, reset counters:**
   ```bash
   proto_reset
   proto_tx_run 500 300 150
   ```

---

### Notes

- **RX devices operate passively:** Once `proto_rx_start` is running, they automatically respond to DISCOVER_REQ, count TEST_DATA packets, and report back on PER_REQ. No manual commands needed.
- **TX device coordinates:** Use `proto_tx_run` for a one-shot flow, or manual `proto_send_*` commands for custom timing.
- **No retries:** TEST_DATA packets are sent once with no retry logic. Packet loss is measured as `(expected - reported) / expected * 100%`.
- **Channel selection:** Channels 11–26 are IEEE 802.15.4 valid (11=2405 MHz to 26=2480 MHz).
- **Signature auto-generation:** If not set manually, each device gets a random 32-bit signature on first boot.

radio_test sample: https://github.com/nrfconnect/sdk-nrf/tree/main/samples/peripheral/radio_test
