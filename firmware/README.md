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

The default build keeps the debug shell disabled and preserves the existing
CMSIS-DAP plus target-UART CDC layout. To build a separate debug variant with
an additional Vendor Bulk interface:

```sh
idf.py -B build-debug-shell \
    -D SDKCONFIG=build-debug-shell/sdkconfig \
    -D 'SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.debug-shell.defaults' \
    build
```

The same selection is available in `idf.py menuconfig`: set TinyUSB's Vendor
interface count to 2, then enable `AirDAP USB device > Enable the USB Vendor
Bulk debug shell`. The checked-in profile is preferred for reproducible
builds. CMake stops with an error if the AirDAP option is enabled while TinyUSB
has fewer than two Vendor interfaces. The CDC count remains one.

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

The build also verifies the fixed 16 MiB OTA layout and rollback configuration.
That gate can be rerun directly:

```sh
python tools/verify_ota_layout.py \
    --partition-table partitions.csv \
    --sdkconfig sdkconfig
```

To flash and monitor a connected board:

```sh
idf.py -p <serial-port> flash monitor
```

The first-stage bootloader is stored in the ESP32-S3 mask ROM and is therefore
not part of this repository. The application currently provides:

- CMSIS-DAP v2 on a vendor-specific USB Bulk interface;
- Microsoft OS 2.0 descriptors for WinUSB binding on Vendor interfaces;
- CDC ACM bridging to target UART1 on GPIO17/GPIO18;
- an optional, independent Vendor Bulk debug shell;
- an SPI2 half-duplex SWD backend on GPIO12/GPIO13/GPIO14;
- target reset, power/status GPIO, VTref, and USB VBUS monitoring.

The USB serial is derived from the eFuse base MAC. VID `0x303A` and PID
`0x4021` are development identifiers; product firmware must use identifiers
the project is authorized to ship. The checked-in layout is for the confirmed
16 MiB module and provides two 4 MiB OTA application slots. Secure Boot, Flash
Encryption, authenticated updates, and networking remain deferred.

CMSIS-DAP uses 512-byte internal buffers and advertises a 508-byte packet
limit. At full-speed USB this keeps the largest response from ending on an
exact 64-byte endpoint boundary, so TinyUSB cannot leave an automatic ZLP for
the following DAP transaction.

## Development USB OTA

The first rollout from the former single-`factory` layout requires one complete
serial flash. This installs the 16 MiB flash header, bootloader rollback
support, partition table, initial OTA metadata, and the application in
`ota_0`:

```sh
idf.py -p <airdap-programming-port> flash
```

An application-only update cannot migrate an existing device's partition
table. Do not use the Python updater until this baseline flash has completed
successfully.

After the baseline is installed, build the next application and update over
the normal AirDAP USB connection without pressing reset or GPIO0:

```sh
python -m pip install pyusb
python tools/airdap-update.py --serial ADP-001122334455 build/airdap.bin
```

When only one AirDAP is connected, `--serial` may be omitted. Stop pyOCD,
OpenOCD, debuggers, and other processes that have claimed the CMSIS-DAP
interface before running the command. On Linux the user needs an appropriate
udev rule; on Windows interface 0 must retain its WinUSB binding.

The tool queries the inactive-slot capacity, disconnects the SWD debug port,
uploads sequential 496-byte chunks, validates and selects the image, requests
a software restart, then waits for the same USB serial and reports its running
version. It writes only `airdap.bin`; it does not replace the bootloader,
partition table, NVS, PHY data, or OTA metadata partition directly.

An incomplete, rejected, or physically disconnected upload is aborted without
selecting the inactive slot. A committed image is initially `PENDING_VERIFY`;
the application confirms it only after board, voltage, SWD, and USB
initialization succeed. If it resets before confirmation, the ESP-IDF
bootloader rolls back to the prior valid slot.

If a USB transfer fails after only part of a variable-length command was
received, firmware expires the abandoned frame after 250 ms. The updater waits
300 ms before recovery, sends `OTA_ABORT`, and drains any stale response before
reporting the original error. Restart the same command from offset zero; this
development protocol does not resume partial uploads.

This is an unauthenticated development interface. Anyone with physical USB
access and a valid ESP32-S3 application image can replace the firmware. It is
not a product OTA security boundary and provides no signing, encryption,
authorization, resume, or network update support. Follow
[`test/hil/usb_ota.md`](test/hil/usb_ota.md) before relying on update and
rollback behavior on hardware.

## Optional Vendor Bulk debug shell

The debug build preserves the existing USB assignments and appends the shell:

- interfaces 0, 1, and 2 remain CMSIS-DAP and `AirDAP Target UART`;
- interface 3 is `AirDAP Debug Shell`, using Bulk OUT `0x04` and Bulk IN
  `0x84`.

Install PyUSB, then open the interface with the checked-in host tool:

```sh
python -m pip install pyusb
python tools/airdap-shell.py --serial ADP-001122334455
```

When only one AirDAP is connected, `--serial` may be omitted. Press Ctrl-] or
Ctrl-D to leave the local tool; Ctrl-C is forwarded to cancel the current
firmware input line. In interactive mode, Tab completes a unique command name,
Left/Right, Home/End, and Delete edit the current line, and Up/Down browses the
eight most recent commands from the current session. When the input line is not
empty, history navigation only visits commands with that prefix. Moving down
past the newest matching entry restores the unsubmitted input line. Commands can
also be run non-interactively:

```sh
python tools/airdap-shell.py -c help -c status -c "swd-idcode 100"
```

`restart` may also be used with `-c`, but it must be the final command because
the device disconnects after its acknowledgement is delivered.

The host tool makes Vendor Bulk communication behave like a raw text terminal.
The firmware accepts printable ASCII, CR/LF line endings, backspace/delete,
Ctrl-C, Tab, and ANSI navigation sequences. Available commands are:

- `help` — list commands;
- `status` — print `target_mv`, `usb_vbus_mv`, `uptime_ms`, and `free_heap`;
- `swd-idcode [clock_khz]` — reset the SWD line, select SWD, and read the
  target DP IDCODE at 100 kHz by default; accepted clocks are 100–10,000 kHz;
- `restart` — wait for the acknowledgement transfer to complete, then restart
  AirDAP; a bounded transfer timeout leaves the firmware running.

Stop OpenOCD or any other CMSIS-DAP client before running `swd-idcode`. The
debug shell and CMSIS-DAP worker share the same physical SWD engine; concurrent
commands would interleave separate target transactions. The command releases
SWDIO after every attempt, including failures.

After `airdap-shell` starts a session, normal ESP application logs are mirrored
to the Vendor Bulk interface and continue to use the configured primary
console. When a log arrives during editing, the shell restores the prompt,
current input, and cursor after printing it. Mirroring uses a bounded queue and
may drop burst logs instead of blocking application tasks. ROM, bootloader,
early application messages, and direct standard output are not captured by the
Vendor interface. The shell is intended for physically connected development
systems: it has no authentication and exposes only a bounded, read-only SWD DP
IDCODE diagnostic. It deliberately provides no arbitrary DP/AP access, target
memory access, programming, persistent history, or dynamic log-control commands.

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
    bootloader_artifact ota_layout board voltage_monitor swd_protocol dap_protocol \
    dap_ota dap_stream ota_manager app_main target_uart usb_descriptors debug_shell_input \
    debug_shell_swd_probe debug_shell_tx_state airdap_shell airdap_update wired_hil; do
    cmake -S "test/unit/$suite" -B "build-host/$suite"
    cmake --build "build-host/$suite"
    ctest --test-dir "build-host/$suite" --output-on-failure
done
```

These tests prove GPIO ordering, bootloader artifact-contract validation, ADC
scaling, SWD transaction framing, CMSIS-DAP and OTA command framing, OTA state
transitions, stale USB-frame recovery, host update ordering, UART line-coding
mapping, both compile-time USB descriptor variants, bounded shell input, the
bounded SWD IDCODE command flow, debug TX completion state, host tools, and
wired HIL helper's protocol checks. They do not prove USB enumeration,
physical flash persistence, bootloader rollback on a board, or electrical SWD
timing.
Follow `test/hil/wired.md` on a populated AirDAP board before marking roadmap
Stage 1 complete.
