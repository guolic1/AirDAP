# AirDAP firmware

This directory contains the ESP-IDF project for the ESP32-S3 used by AirDAP.
It builds the standard ESP-IDF second-stage bootloader with an AirDAP hook, the
wired CMSIS-DAP application, and its partition table.

The ROM first-stage bootloader is fixed in the ESP32-S3. The project extends
the ESP-IDF second-stage bootloader only to place target-facing GPIOs in their
safe states before ESP-IDF initializes and validates the application image:

- SWCLK low and SWDIO high impedance;
- SWDIO direction set to target-to-AirDAP;
- target reset released;
- target power/status GPIO9 released as an open-drain output;
- target UART TX idle high and RX high impedance.

## Build

Activate an ESP-IDF environment, then run:

```sh
cd firmware
idf.py build
```

The main images are generated at:

- `build/bootloader/bootloader.bin`
- `build/partition_table/partition-table.bin`
- `build/airdap.bin`

`idf.py build` runs the bootloader artifact gate automatically. While the
ESP-IDF environment is active, it can also be rerun directly:

```sh
python tools/verify_bootloader.py --build-dir build
```

This gate checks that the AirDAP hooks are strong symbols in the bootloader
ELF, that `call_start_cpu0()` calls `bootloader_before_init()` before
`bootloader_init()`, that the target is ESP32-S3, and that region protection,
the 9-second RTC watchdog, and full application image validation remain
enabled.

To flash and monitor a connected board:

```sh
idf.py -p <serial-port> flash monitor
```

The first-stage bootloader is stored in the ESP32-S3 mask ROM and is therefore
not part of this repository. The application currently provides:

- CMSIS-DAP v2 on a vendor-specific USB Bulk interface;
- Microsoft OS 2.0 descriptors for WinUSB binding on interface 0;
- CDC ACM bridging to target UART1 on GPIO17/GPIO18;
- an SPI2 half-duplex SWD backend on GPIO12/GPIO13/GPIO14;
- target reset, power/status GPIO, VTref, and USB VBUS monitoring.

The USB serial is derived from the eFuse base MAC. VID `0x303A` and PID
`0x4021` are development identifiers; product firmware must use identifiers
the project is authorized to ship. Secure Boot, OTA partitions, and networking
remain deferred to their roadmap stages. The final flash size and OTA layout
must be selected after the exact ESP32-S3-MINI-1U ordering code is fixed.

CMSIS-DAP uses 512-byte internal buffers and advertises a 508-byte packet
limit. At full-speed USB this keeps the largest response from ending on an
exact 64-byte endpoint boundary, so TinyUSB cannot leave an automatic ZLP for
the following DAP transaction.

## Host unit tests

Run these suites in a clean host shell before activating ESP-IDF. ESP-IDF adds
cross-toolchain programs to `PATH`, while these tests must use the host compiler
and linker.

The bootloader GPIO test uses a fake GPIO LL backend to verify final pin states
and the order used to avoid output glitches:

```sh
cmake -S test/unit/bootloader_gpio -B build-host/bootloader_gpio
cmake --build build-host/bootloader_gpio
ctest --test-dir build-host/bootloader_gpio --output-on-failure
```

This test does not replace a board-level measurement of pin levels during
reset and boot.

The other hardware-independent tests use the same pattern:

```sh
for suite in \
    bootloader_artifact board voltage_monitor swd_protocol dap_protocol target_uart \
    usb_descriptors wired_hil; do
    cmake -S "test/unit/$suite" -B "build-host/$suite"
    cmake --build "build-host/$suite"
    ctest --test-dir "build-host/$suite" --output-on-failure
done
```

These tests prove GPIO ordering, bootloader artifact-contract validation, ADC
scaling, SWD transaction framing, CMSIS-DAP command framing, UART line-coding
mapping, the actual composite USB descriptor bytes, and the wired HIL helper's
protocol checks. They do not prove USB enumeration or electrical SWD timing.
Follow `test/hil/wired.md` on a populated AirDAP board before marking roadmap
Stage 1 complete.
