# Wired firmware hardware-in-the-loop acceptance

Run this checklist on the exact populated AirDAP PCB and record the board
revision, ESP32-S3 module ordering code, target MCU, host OS, tool versions,
and measurements. Passing the firmware build or host unit tests is not a
substitute for this checklist.

## Automated evidence helper

Install current pyOCD, PyUSB, and pyserial releases in an isolated Python
environment. On Windows, PyUSB also needs a libusb backend capable of opening
the WinUSB-bound Vendor interfaces. On Linux, grant the user access to the
device with an appropriate udev rule.

With target UART TX/RX connected as a loopback and a known binary reference
image selected for the target, run:

```sh
python test/hil/wired_smoke.py \
    --serial ADP-001122334455 \
    --board-revision <pcb-revision> \
    --module-ordering-code <esp32-s3-module-ordering-code> \
    --reference-target <target-board-and-mcu> \
    --exercise-reset \
    --cdc-port <target-uart-com-port> \
    --program-image <reference-image.bin> \
    --target <pyocd-target-type> \
    --base-address <target-flash-address> \
    --reliability-cycles 100 \
    --output <evidence.json>
```

This command performs destructive sector erase/program operations on the
reference target. It validates the USB interface and endpoint layout, USB and
DAP serial agreement, DAP capabilities, the 508-byte response boundary,
stable DP IDCODE, reset command, CDC loopback line codings, a 500 kHz/1 MHz/
5 MHz program/readback sweep, and the requested reconnect/program/readback
cycles. The JSON records whether the optional Vendor Bulk debug interface is
present. The JSON also includes tool versions, image hash, timings, and
hardware identifiers. Run the command again with the recorded `--serial` after
an unplug/replug to prove serial stability.

The helper cannot observe analog or electrical behavior. Oscilloscope traces,
ADC comparison, contention checks, GPIO9 open-drain behavior, reverse-current
tests, and Windows driver binding still require the manual steps below.

## 1. Boot safety

Follow `bootloader.md`. Capture SWCLK, SWDIO, SWDIO_DIR, target nRESET control,
GPIO9, and target UART TX from reset assertion until the application starts.
Confirm that no unsafe pulse occurs before `bootloader_before_init()`.

## 2. Flash and startup

```sh
idf.py -p <airdap-programming-port> flash monitor
```

Confirm that the log reports target VTref and USB VBUS without an ADC or GPIO
initialization error. Record the measured voltages at the ADC pins and compare
them with the logged values, including divider ratios and tolerances.

## 3. USB descriptors and Windows binding

Confirm one composite device with VID `303A`, PID `4021`, and a stable serial
`ADP-<12 hex digits>` appears. Verify:

- interface 0 is class `FF`, subclass `00`, protocol `00`;
- its interface string contains `CMSIS-DAP`;
- Bulk OUT `0x01` precedes Bulk IN `0x81`, both with 64-byte max packets;
- interface 0 binds WinUSB through the Microsoft OS 2.0 descriptor;
- interfaces 1/2 enumerate as CDC ACM with endpoints `0x82`, `0x03`, `0x83`;
- when the debug-shell build is selected, interface 3 enumerates as class
  `FF/00/00`, is named `AirDAP Debug Shell`, and has Bulk OUT `0x04` followed
  by Bulk IN `0x84`;
- interfaces 0 and 3 bind WinUSB through the Microsoft OS 2.0 descriptor;
- unplug/replug preserves both the USB serial and driver bindings.

Repeat descriptor inspection on Linux with `lsusb -v` when available.

## 4. CMSIS-DAP and SWD

With a known-good Cortex-M reference target and a current limiting supply:

1. Run `pyocd list` and confirm the AirDAP CMSIS-DAP v2 interface is selected.
2. Connect at 500 kHz, read DP IDCODE repeatedly, and record the value.
3. Exercise DP and AP reads/writes, including a deliberately disconnected
   target to observe WAIT/FAULT/error handling.
4. Erase, program, and verify a known reference image.
5. Repeat connect/program/verify at 1 MHz and 5 MHz. Treat 10 MHz as an
   optimization result, not a Stage 1 requirement.
6. Perform at least 100 reconnect/program/verify cycles and record failures.

Probe SWCLK, SWDIO, and GPIO14 during read and write transfers. Confirm the
external direction signal changes only across the configured turnaround and
that SWDIO contention is absent.

## 5. CDC target UART

Connect GPIO17/GPIO18 to a reference UART or loopback fixture. Verify both
directions at 9,600, 115,200, and 1,000,000 baud. Change baud, data bits,
parity, and stop bits from the host and confirm supported CDC line coding is
applied. Confirm unsupported mark/space parity is rejected without crashing or
changing the last valid configuration.

## 6. Optional Vendor Bulk debug shell

For a build made with `sdkconfig.debug-shell.defaults`, run:

```sh
python tools/airdap-shell.py --serial <ADP-serial>
```

Confirm this does not open or change `AirDAP Target UART`. Run `help` and verify
that `help`, `version`, `status`, `swd-idcode`, and `restart` are listed in cyan and the
descriptions remain in the terminal's default color. Verify the prompt is cyan,
a successful `status` result is green, `status extra` usage guidance is yellow,
and an unknown command is red. Type `sta`, press Tab, and verify it completes to
`status `. On an empty line, press Tab and verify all five commands are listed.
Type `s`, press Tab, and verify `status` and `swd-idcode` are listed in cyan on
separate lines before the prompt restores `s`. Use Left/Right, Home/End,
Backspace, and Delete to edit text at the beginning, middle, and end of a command
without prompt-width or cursor drift. Submit multiple commands, type a prefix,
and verify Up/Down only visits matching history; verify moving down past the
newest match restores the unsubmitted draft and cursor. Reconnect with
`--color never` and verify the same interaction is plain text, then return to the
default color mode. Reconnect the shell and verify the previous session's
history is unavailable. While application logs are arriving, pause with the
cursor in the middle of an unfinished command and verify each log retains its
original formatting and is followed by the intact colored prompt, input, and
cursor position. Run `version` and confirm its `firmware_version` value matches
both the exact Git tag on the built commit or, when untagged, its seven-character
Git hash and the version reported by the USB OTA query. Run `status` and verify
decimal target voltage, USB VBUS voltage, uptime, and free heap fields. With
OpenOCD and other CMSIS-DAP clients closed, run `swd-idcode 100` against the
known-good reference target. Confirm it reports the same nonzero DP IDCODE
recorded in section 4, then repeat once with the target disconnected and confirm
the command reports a red error and the target-side SWDIO line is released.
Confirm normal ESP application logs still appear on the primary console.
Finally, run `restart`, verify the yellow acknowledgement is received in full
before disconnect, and confirm the composite device re-enumerates with the same
serial number. Repeat with Bulk IN deliberately left unread past the firmware's
bounded timeout and confirm AirDAP does not restart.

## 7. Reset and target power

- Issue `DAP_ResetTarget` and verify a 1 ms asserted reset pulse at the target.
- Exercise `DAP_SWJ_Pins` with nRESET selected and verify asserted/released
  polarity at both GPIO41 and the target connector.
- With USB input present and absent, exercise GPIO9 only as open drain. Confirm
  that firmware never drives it push-pull high.
- Test target self-power and AirDAP-provided power separately. Confirm no
  reverse current path and verify the TPS2116 status readback interpretation.
- On shutdown, confirm SWCLK is low, SWDIO is high impedance, reset is released,
  and GPIO9 can actively disable AirDAP-provided target power.

## Exit evidence

Store USB descriptor captures, oscilloscope traces, voltage measurements,
pyOCD logs, programmed image hash, UART results, and the 100-cycle result with
the tested board revision. Only then may roadmap Stage 1 be marked complete.
