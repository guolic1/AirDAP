# Bootloader board-level verification

The host unit test proves the GPIO programming contract, but it cannot prove
physical levels between reset release and application startup. Run this check
on the AirDAP PCB before considering the bootloader hardware-safe.

## Equipment and setup

- AirDAP PCB with the exact ESP32-S3-MINI-1U ordering code recorded;
- oscilloscope or logic analyzer referenced to AirDAP ground;
- target disconnected for the first run;
- probes on ESP32 EN, GPIO9, GPIO12, GPIO13, GPIO14, GPIO17, GPIO18, and GPIO41.

Flash `bootloader.bin`, `partition-table.bin`, and `airdap.bin` from one clean
build. Before flashing, run `python tools/verify_bootloader.py --build-dir
build` in the activated ESP-IDF environment and retain its passing output with
the build artifacts. Trigger the capture on the rising edge of EN and retain
the interval from reset release through the `AirDAP firmware started` log
message.

## Required observations

| Signal | Required result after the AirDAP hook runs |
|---|---|
| GPIO12 / SWCLK | Low, without a positive pulse |
| GPIO13 / SWDIO | High impedance |
| GPIO14 / SWDIO DIR | Low, target-to-AirDAP |
| GPIO41 / target reset control | Low; target nRESET remains released |
| GPIO9 / power status-control | Open-drain released; it must not source the shared net |
| GPIO17 / target UART TX | High, without a negative pulse |
| GPIO18 / target UART RX | High impedance |

Repeat the capture for power-on reset, EN reset, and software reset. Then test
with no valid application image on a recoverable development board; the target
control pins must remain safe while the bootloader reports the image failure
or resets through its watchdog.

High impedance must be verified with a known weak external pull or equivalent
current measurement, not inferred only from a logic level. Also inspect the
pre-hook ROM window. If any target-facing signal requires a defined level from
the instant reset is released, enforce that level in hardware because project
bootloader code cannot run during the mask-ROM stage.
