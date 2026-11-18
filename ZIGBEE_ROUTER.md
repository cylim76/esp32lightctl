# ESP32-C6 dedicated Zigbee Router

This target repurposes a spare ESP32-C6 as an always-powered Zigbee Router for
the esp32lightctl Coordinator. It does not start Wi-Fi, HTTP, scheduling, IR,
RTC, sensors, or RainMaker, so those services cannot compete for the 2.4 GHz
radio or allocate background tasks.

## Status

- Source implementation: IMPLEMENTED
- ESP-IDF 5.5.1 / esp-zigbee-lib 2.0.5 build: BUILD PASS (2026-11-18)
- Static size report: SOFTWARE PASS (Flash code 453,754 B; static D/IRAM 84,776 B)
- USB-UART power-on/reset stability: HARDWARE PASS (2026-11-18, 20 dBm build)
- Join to the esp32lightctl Coordinator: HARDWARE PASS (2026-11-18)
- Aqara QBKG25LM communication through the Router: HARDWARE PASS, short observation only
- Long-term routing and power-loss recovery: NOT TESTED

## Network settings

- Role: Zigbee Router (not Coordinator)
- Channel: 15
- Maximum children: 16
- Requested TX power: 20 dBm
- Sleepy-child timeout: 16384 minutes
- Wi-Fi: disabled
- Zigbee persistence partition: `zigbee_nvs`

The Router cannot create the office Zigbee network. The existing original
ESP32-C6 gateway remains the only Coordinator.

## Build

From an ESP-IDF 5.5.1 command prompt:

```bat
call D:\RPA\esp-idf-5.5.1\export.bat
cd /d D:\RPA\esp32lightctl
build-router.bat
```

## First conversion of the spare board

The `fresh` argument deliberately erases only the board connected to the named
COM port. Use it once when converting the spare gateway board into a Router so
old Coordinator network state cannot be restored accidentally.

```bat
flash-router.bat COM12 fresh
monitor-router.bat COM12
```

Do not run `fresh` against the original office gateway.

## Join and placement test

1. Keep the original gateway powered and verify its Zigbee network is on
   channel 15.
2. In the gateway Web page, open Permit Join for 180 seconds.
3. Power the freshly flashed Router. It scans channel 15 every 10 seconds until
   it joins.
4. Confirm the Router serial log reports `Joined Zigbee network` and a nonzero
   short address.
5. Put the Router near the Aqara D1, with its PCB antenna clear of metal.
6. Rejoin the D1 while Permit Join remains open so it can select the Router as
   its parent.
7. Test physical reports, Web control, 60-second state reads, gateway reboot,
   Router reboot, and at least 24 hours of stability.

Build success does not count as Router or Aqara hardware success.

## Build artifact record (2026-11-18)

| File | Address | Size | SHA-256 |
|---|---:|---:|---|
| `bootloader/bootloader.bin` | `0x0000` | 22,496 B | `330420aba17931e3177ead517349b2a984e0f241467d49a6a0b02da058a470ca` |
| `partition_table/partition-table.bin` | `0x8000` | 3,072 B | `d5b5749234b698a3e37aa9f46cb028d64048e38d4a37cd98d4071e80e9095884` |
| `esp32c6_zigbee_router.bin` | `0x10000` | 543,424 B | `f6583d6bda9e88030ef03ec06c5e367cd06cea98e784251aa003daff8e726fe5` |

The application uses 453,754 bytes of Flash code and 84,776 bytes of static
D/IRAM (`.data` 9,652 B, `.bss` 10,416 B, DIRAM `.text` 64,708 B). The
1.5 MiB factory application partition has 1,044,640 bytes (66%) free. Runtime
heap, RF routing performance, child capacity, TX power and thermal stability
remain hardware `NOT TESTED`.

During the first hardware attempt, the board repeatedly triggered the brownout
detector while using an unstable USB path. Replacing the cable removed the
repeated resets, and the 20 dBm build subsequently joined the Coordinator and
improved observed D1 communication stability. This short observation does not
yet prove long-term RF, thermal, routing or power-loss recovery stability.
