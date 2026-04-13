Site Survey

This firmware is designed for the XIAO nRF54L15.
It is built off of Nordics radio_test sample. A link below provides more information about the radio_test sample and how to use it.

Changes from the original radio_test sample:
- Ported for the XIAO nRF54L15
- Added custom IEEE 802.15.4 protocol with device signatures for PER testing
- Each device has a unique 32-bit hardware signature and can auto-join into a receiver role over the air
- Transmitter can discover multiple receivers, send numbered test packets, and collect per-device PER
- Receivers auto-respond to discovery and PER requests (no manual intervention required)
- Enabled 2-byte CRC (16-bit FCS) for IEEE802.15.4
- Boot defaults now start all boards in IEEE 802.15.4 mode on channel 15

## Coordinator Workflow

The intended workflow is now:

- One board connected to the host PC becomes the **main** board.
- All other boards boot with the same firmware and start in an **unassigned** state.
- The main board sends a discover request ("who's out there?").
- Unassigned boards that hear the discover request auto-join as receivers.
- Receiver IDs are based on each board's internal nRF54L15 serial number (hardware-derived signature).
- Once a slave board has joined and is ready to receive test data, its ready LED starts blinking.
- After all expected slave boards are discovered, the main board is switched into **test transmitter** mode and runs the PER test.

### Set Up The Main Board

On the board connected to your host PC:

```bash
node_mode coordinator
node_status
```

Optional (only needed if you changed radio settings previously):

```bash
data_rate ieee802154_250Kbit
start_channel 15
```

What this does:

1. Puts the host-connected board into coordinator mode so it can discover slave boards.
2. Prints the current node state so you can confirm it is ready.
3. Optional commands can force IEEE mode/channel if you changed them.

### Prepare Slave Boards

For each receiver board:

1. Flash the same firmware.
2. Power it on.
3. By default it boots in IEEE mode on channel 15 (same as coordinator default).
4. If you changed channel on the coordinator, set the same channel on receivers as well.
5. No button press is required; it will auto-join when it hears discover.

When a slave board is discovered successfully, it enters receiver mode using its hardware-derived signature and its ready LED starts blinking to show it is ready to receive data.

Note: after any board reset/power cycle, the board returns to unassigned mode and must be discovered again.

### Discover All Available Slave Boards

Use the main board to send a discover request before starting the PER test:

```bash
node_mode coordinator
discover_list_clear
proto_send_discover 200
discover_status
```

What to look for:

1. The main board reports the number of discovered peers.
2. Each discovered slave board starts blinking its ready LED.
3. `discover_status` prints a CSV-style list of discovered slave signatures.

If you run `discover_list_clear`, the main board now tells each discovered slave to return to the unassigned default state before clearing the local peer list. On the slave, this stops the ready LED blink and puts it back into the same state as a fresh boot before discovery.

Do not start the PER test until all expected slave boards have been discovered and are blinking ready.

### Run The PER Test

Once all slave boards are discovered and blinking ready, run a single command:

```bash
proto_test_run 100 200 200
```

This will:

1. Use the current discovered peer list without rediscovering.
2. Run the main-board TEST_DATA/PER flow.
3. Run delegated round-robin tests for discovered peers.
4. Emit machine-readable `PER_REPORT` lines for easy host logging.

### Run A Round-Robin PER Sweep

Use this flow when you want one board attached to the PC to stay as the single logger while each discovered slave takes a turn as the transmitter.

Type the following on the PC-attached main board, in this order.

#### Step 1: Put the PC-attached board into coordinator mode

```bash
node_mode coordinator
```

This keeps the PC-attached board listening so it can:

1. Receive delegated PER reports from the temporary transmitter board.
2. Participate as one of the receivers during each delegated test.

#### Step 2: Clear any old discovered list and force all slaves back to unassigned

```bash
discover_list_clear
```

Use this at the start of a new session so you know exactly which boards are part of the test.

#### Step 3: Discover the current set of slave boards

```bash
proto_send_discover 200
discover_status
```

Check `discover_status` and confirm:

1. The expected number of slave boards are listed.
2. Each listed slave has been discovered.
3. The slave LEDs are blinking ready.

At this point, the discovered list is set up and ready. You can stop here if you only want to prepare the list before starting any test.

#### Step 4: Start the full test run

```bash
proto_test_run 100 200 200
```

Meaning of the arguments:

1. `100`: number of TEST_DATA packets each delegated transmitter sends.
2. `200`: wait time in ms after TEST_DATA before PER collection starts.
3. `200`: retry interval in ms while waiting for PER responses and clear confirmations.

This will:

1. Keep the PC-attached main board in coordinator mode so it can receive logs and also participate as one of the receivers in each delegated PER test.
2. Run the local main-board test first.
3. Walk the current discovered peer list one board at a time.
4. Ask each discovered board to become the temporary test transmitter.
5. Have that delegated board run its own discover, include the PC-attached main board in that discover, send TEST_DATA, collect PER from the other nodes, clear their PER counters, and report each PER result back to the original main board.
6. Print `PER_REPORT` lines for each result, followed by delegate completion lines.

#### What you should see on the main board

For each discovered slave acting as the delegated transmitter, you should see output like:

```text
ACK_START,proto_test_run
PER_REPORT,123456,0xYYYYYYYY,0xXXXXXXXX,100,99
Round-robin: delegate 0xXXXXXXXX complete, reports=1
ACK_END,proto_test_run,OK
```

Interpretation:

1. `delegate=0xXXXXXXXX`: the slave board currently acting as transmitter.
2. `peer=0xYYYYYYYY`: the receiver board being reported. This can be the PC-attached main board or another discovered slave.
3. `rx=99 expected=100`: that receiver got 99 out of 100 packets, so PER is 1%.

#### Full Example: Exact Commands To Type

If you want a clean end-to-end round-robin session from scratch, type this on the PC-attached main board:

```bash
node_mode coordinator
discover_list_clear
proto_send_discover 200
discover_status
proto_test_run 100 200 200
```

If you want to stop after setting up the discovered list, type only:

```bash
node_mode coordinator
discover_list_clear
proto_send_discover 200
discover_status
```

If you want to run the full test using the current discovered list, type:

```bash
proto_test_run 100 200 200
```

If you want to build the discovered list first, then later run the round-robin sweep, use these two separate phases:

Phase 1, build the discovered list:

```bash
node_mode coordinator
discover_list_clear
proto_send_discover 200
discover_status
```

Phase 2, run tests using that existing list:

```bash
proto_test_run 100 200 200
```

Use this shortest end-to-end sequence:

```bash
node_mode coordinator
discover_list_clear
proto_send_discover 200
discover_status
proto_test_run 100 200 200
```

#### How To Stop The Round-Robin Sweep

If you need to stop the round-robin sweep while it is running, type:

```bash
cancel
```

That stops the current wait loop on the PC-attached board.

The local main-board run and delegated round-robin are both included in `proto_test_run`.

### Useful Coordinator Commands

| Command | Purpose | Example |
|---------|---------|---------|
| `node_mode coordinator` | Make the host-connected board the coordinator | `node_mode coordinator` |
| `discover_list_clear` | Return discovered peers to unassigned state, then clear the discovered peer list before a new discover round | `discover_list_clear` |
| `discover_status` | Print discovered peers in CSV-friendly format (`DISCOVER_STATUS,index,signature`) | `discover_status` |
| `proto_test_run [pkts] [wait_ms] [retry_ms]` | Run local test plus delegated round-robin in one command | `proto_test_run 100 200 200` |
| `node_mode receiver` | Force a discovered board back into receiver mode | `node_mode receiver` |
| `node_mode unassigned` | Put a board back into unassigned mode | `node_mode unassigned` |
| `node_status` | Print node mode, hardware signature, active signature, and assigned receiver ID | `node_status` |
| `node_factory_reset` | Clear saved assignment and return the board to unassigned mode | `node_factory_reset` |

### Machine-Readable UART Output

The host/Pi parser can key off these lines:

- `ACK_START,<command>`
- `ACK_END,<command>,OK|ERR`
- `DISCOVER_STATUS,index,signature`
- `PER_REPORT,time_ms,dst_id,src_id,sent_packets,received_packets`

Runtime log mode switch:

- `log_mode minimal` keeps UART output focused on ACK, `DISCOVER_STATUS`, and `PER_REPORT` lines.
- `log_mode verbose` re-enables human-readable progress logs for debugging.

Recommended host flow:

```bash
node_mode coordinator
log_mode minimal
discover_list_clear
proto_send_discover 200
discover_status
proto_test_run 100 200 200
```

### Raspberry Pi Logger Script

Use `simple_serial_logger.py` to automate discovery retries and save CSV logs:

```bash
python3 simple_serial_logger.py \
   --port /dev/ttyACM0 \
   --expected-devices 3 \
   --packets 100 \
   --wait-ms 200 \
   --retry-ms 200 \
   --discover-wait-ms 200
```

Outputs:

- `serial_logs/session_<timestamp>.log` (raw UART)
- `serial_logs/discovered_<timestamp>.csv`
- `serial_logs/per_<timestamp>.csv`

## IEEE 802.15.4 Protocol for Multi-Device Testing

### Protocol Overview

The firmware implements a request/response protocol for coordinating PER tests across multiple devices:

**Commands:**
- **DISCOVER_REQ** (TX broadcast): "Who wants to participate in testing?"
- **DISCOVER_RSP** (RX unicast): "I'm here, my signature is 0xXXXXXXXX"
- **TEST_DATA** (TX broadcast): Numbered packets for PER measurement
- **PER_REQ** (TX unicast): "Send me your received packet count"
- **PER_RSP** (RX unicast): "I received N packets out of expected M"
- **CLEAR_PER_REQ** (TX unicast): "Clear your PER counter and confirm"
- **CLEAR_PER_RSP** (RX unicast): "My PER counter has been cleared"

### Quick Start: Discover Then Run A Test

#### On the host-connected board
```bash
node_mode coordinator
discover_list_clear
proto_send_discover 200
discover_status
```

Optional if you changed settings previously:

```bash
data_rate ieee802154_250Kbit
start_channel 15
```

#### On each receiver board
- Power the board.
- No button press required.
- Wait for the ready LED to blink, indicating the board has been discovered and is ready.

#### Run the test from the host-connected board only
```bash
proto_test_run 100 200 200
```

**What this does:**
1. Main board broadcasts DISCOVER_REQ → all available slave boards reply with their serial-based signatures
2. Main board waits 200 ms to collect all DISCOVER_RSP messages
3. Main board runs local TEST_DATA and PER collection
4. Main board runs delegated round-robin tests for discovered peers
5. Firmware emits machine-readable `PER_REPORT` lines for host logging

**Output example:**
```
ACK_START,proto_test_run
PER_REPORT,123456,0x87654321,0x12345678,100,97
PER_REPORT,123789,0xAABBCCDD,0x12345678,100,99
ACK_END,proto_test_run,OK
```

Interpretation:
- TX sent 100 TEST_DATA packets
- Device 0x87654321 received 97 → **PER = 3%**
- Device 0xAABBCCDD received 99 → **PER = 1%**

---

### Manual Step-by-Step Control

If you want to debug the flow in smaller steps, use the host-connected board only:

```bash
node_mode coordinator
discover_list_clear
proto_send_discover 200
discover_status

proto_test_run 100 200 200
```

What this manual sequence gives you:

1. A clean discover round with a persistent peer list.
2. An explicit pause to inspect discovered peers before transmitting test data.
3. A single command test run with host-parseable ACK boundaries and PER rows.
4. A way to return all currently discovered peers back to the default unassigned state by running `discover_list_clear`.
5. The ability to stop active loops with `cancel`.

---

### Command Reference

| Command | Purpose | Example |
|---------|---------|---------|
| `data_rate ieee802154_250Kbit` | Enable 802.15.4 mode (required for protocol) | `data_rate ieee802154_250Kbit` |
| `start_channel <11–26>` | Set IEEE channel (11=2405MHz, 15=2425MHz, 20=2450MHz, 26=2480MHz) | `start_channel 15` |
| `discover_list_clear` | Return current discovered peers to unassigned state, then clear the discovered peer list | `discover_list_clear` |
| `node_mode coordinator` | Put the host-connected board into coordinator mode | `node_mode coordinator` |
| `discover_status` | Print discovered peers in CSV-friendly format (`DISCOVER_STATUS,index,signature`) | `discover_status` |
| `proto_test_run [pkts] [wait_ms] [retry_ms]` | Run local test plus delegated round-robin in one command | `proto_test_run 100 200 200` |
| `proto_round_robin_run [pkts] [wait_ms] [retry_ms]` | Run delegated-only round-robin tests (if needed separately) | `proto_round_robin_run 100 200 200` |
| `proto_send_discover [wait_ms]` | Broadcast "who's there?" and listen for replies | `proto_send_discover 300` |
| `proto_send_test_data [pkts]` | Send numbered test packets | `proto_send_test_data 100` |
| `proto_collect_per <expected> [wait_ms] [retry_ms]` | Keep requesting PER, then keep requesting PER clear, until all discovered peers respond and confirm clear or `cancel` | `proto_collect_per 100 200 200` |
| `proto_send_per_req <sig> <expected> [wait_ms]` | Query specific device's RX count and wait for reply | `proto_send_per_req 0x87654321 100 120` |
| `proto_status` | Print all counters, discovered peers, and PER | `proto_status` |
| `proto_reset` | Clear counters (between test runs) | `proto_reset` |

---

### Understanding PER Output

For host logging, parse `PER_REPORT` lines:

```
PER_REPORT,123456,0x87654321,0x12345678,100,97
```

Fields:

1. `time_ms`: device uptime timestamp in milliseconds.
2. `dst_id`: receiver signature.
3. `src_id`: transmitter signature.
4. `sent_packets`: expected packet count for this report.
5. `received_packets`: packets counted by the receiver.

PER formula:

- `PER = (sent_packets - received_packets) / sent_packets * 100%`

---

### Typical Test Procedure

1. **Set the host-connected board as coordinator:**
   ```bash
   node_mode coordinator
   node_status
   ```

2. **Power the slave boards and let them stay unassigned:**
   They listen for discover requests on channel 15 by default.

3. **Discover the current slave set:**
   ```bash
   discover_list_clear
   proto_send_discover 200
   discover_status
   ```

4. **Run the full test using the current discovered list:**
   ```bash
   proto_test_run 1000 300 150
   ```

5. **Optionally run delegated-only round-robin sweep:**
   ```bash
   node_mode coordinator
   proto_round_robin_run 1000 300 150
   ```

6. **Inspect the results:**
   ```bash
   discover_status
   ```

7. **Repeat for another run if needed:**
   `proto_test_run` will reuse the current discovered peer list until you run `discover_list_clear` or do a new discovery round.

---

### Notes

- **RX devices operate passively:** Once discovered, they automatically respond to DISCOVER_REQ, count TEST_DATA packets, report PER on PER_REQ, and clear their counters on CLEAR_PER_REQ.
- **List clearing also resets slaves:** Running `discover_list_clear` on the main board tells currently discovered slaves to return to the unassigned default state, which also stops the ready LED blink.
- **TX device coordinates:** Use `proto_test_run` for the one-shot flow, or use lower-level commands (`proto_send_test_data`, `proto_collect_per`, `proto_round_robin_run`) when you want explicit control.
- **Round-robin logging:** Use `proto_round_robin_run` when you want the PC-attached board to stay as the single logger while also participating as a receiver and each discovered peer takes a turn as the test transmitter.
- **No retries:** TEST_DATA packets are sent once with no retry logic. Packet loss is measured as `(expected - reported) / expected * 100%`.
- **Channel selection:** Channels 11–26 are IEEE 802.15.4 valid (11=2405 MHz to 26=2480 MHz).
- **Boot channel default:** Boards boot into IEEE 802.15.4 mode on channel 15.
- **Hardware signatures:** Each board uses its hardware-derived signature for discovery and peer tracking.

radio_test sample: https://github.com/nrfconnect/sdk-nrf/tree/main/samples/peripheral/radio_test
