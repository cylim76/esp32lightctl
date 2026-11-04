# ESP RainMaker Classic test plan

## Automated software checks

| Test | Current result |
|---|---|
| ESP-IDF 5.5.1 + RainMaker 1.17.0 dependency build | BUILD PASS (CORE_CLOUD) |
| Node/device graph and MQTT/claim API contract | SOFTWARE TEST PASS |
| Cloud control policy: reject cached init, serialize commands | C TEST BUILD PASS; QEMU execution by CI |
| Individual light ON/OFF routed through Device Control Service | SOFTWARE TEST PASS |
| AC and all-light actions are explicit OFF | SOFTWARE TEST PASS |
| Stale/timed-out Zigbee state excluded from cloud sync | SOFTWARE TEST PASS |
| Embedded Web JavaScript syntax | SOFTWARE TEST PASS |

## Hardware/cloud tests

All items below are **NOT TESTED** until recorded with an ESP32-C6 board and a
real RainMaker account.

1. Self-claim succeeds and credentials survive reboot.
2. User-node mapping completes from Android/iOS app and from CLI.
3. TLS/MQTT reconnects after router loss without affecting Core.
4. Each light can be switched ON and OFF; app state follows physical button
   changes and never accepts stale endpoint cache.
5. All Lights OFF targets every mapped D1 endpoint.
6. AC 1, AC 2 and group send the learned OFF code; app never claims physical
   OFF confirmation.
7. BH1750/SHT31 readings appear only while valid.
8. Disable/reboot/re-enable preserves RainMaker account binding.
9. Record Free Heap, Minimum Free Heap and Largest Free Block before start,
   during self-claim, TLS handshake and steady MQTT operation.
10. Run Wi-Fi + Zigbee pairing + IR learning + RainMaker for 24 hours, then the
    full 7-day stability test.

Evidence to capture: firmware SHA-256, node ID, serial log, app screenshots,
heap minima, task stack high-water marks, reconnect counts and every failure
code. A cloud test is PASS only when both the phone/CLI result and serial log
agree.
