Site Survey

This firmware is designed for the XIAO nRF54L15.
It is built off of Nordics radio_test sample. A link below provides more information about the radio_test sample and how to use it.

Changes from the original radio_test sample:
- Ported for the XIAO nRF54L15
- Added a signature to the beginning of the IEEE802.15.4 payload to identify the source of the payload. This was important for accurate PER testing.
- Enabled 2-byte CRC (16-bit FCS) for IEEE802.15.4

radio_test sample: https://github.com/nrfconnect/sdk-nrf/tree/main/samples/peripheral/radio_test
