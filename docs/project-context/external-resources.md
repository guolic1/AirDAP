# External Resources

## Overview

This file records stable access methods for evidence outside the repository.
Repository design documents may reference these labels when a hardware or
software decision depends on a component specification.

## Additional Resources

| Label | Description | Access Method | Owner / Notes |
|-------|-------------|---------------|---------------|
| `ti-tps2116-datasheet` | TPS2116 power-multiplexer behavior, including the open-drain `ST` output and reverse-current blocking | https://www.ti.com/lit/ds/symlink/tps2116.pdf | Texas Instruments; schematic U2 |
| `ti-tps22919-datasheet` | TPS22919 load-switch `ON` input and output-discharge behavior | https://www.ti.com/lit/ds/symlink/tps22919.pdf | Texas Instruments; schematic U3 |
| `arm-cmsis-dap-usb` | CMSIS-DAP v2 USB Bulk interface class, endpoint ordering, and product-string requirements | https://arm-software.github.io/CMSIS_5/DAP/html/group__DAP__ConfigUSB__gr.html | Arm CMSIS-DAP specification |
| `arm-cmsis-dap-reference` | Reference command processing and SWD error recovery, including pin control without an active debug-port selection and the 33-bit invalid-ACK data-phase backoff | https://github.com/ARM-software/CMSIS-DAP/tree/main/Firmware/Source | Arm CMSIS-DAP reference firmware |
| `tinyusb-ms-os-20-example` | BOS and Microsoft OS 2.0 descriptors used to select WinUSB for a vendor interface | https://github.com/hathach/tinyusb/blob/master/examples/device/webusb_serial/src/usb_descriptors.c | Upstream TinyUSB example |
| `tinyusb-vendor-stream` | Vendor Bulk TX completion behavior, including automatic ZLP generation after a max-packet-sized final endpoint transfer | https://github.com/hathach/tinyusb/blob/0.19.0/src/class/vendor/vendor_device.c | Upstream TinyUSB source matching the managed component |
| `pyocd-cmsis-dap-packet-sizing` | pyOCD uses the probe-reported CMSIS-DAP packet size both as its USB read size and to calculate transfer batching | https://github.com/pyocd/pyOCD/blob/main/pyocd/probe/pydapaccess/dap_access_cmsis_dap.py | pyOCD CMSIS-DAP host implementation |
| `pyocd-programming-api` | Supported session selection and `FileProgrammer` API used by the wired HIL helper before explicit memory readback verification | https://pyocd.io/docs/api_examples.html | Official pyOCD Python API documentation |

## Update History

| Date | Change | Author |
|------|--------|--------|
| 2026-08-29 | Recorded target-power component specifications | Codex |
| 2026-08-29 | Recorded CMSIS-DAP USB and TinyUSB WinUSB descriptor references | Codex |
| 2026-08-29 | Recorded Arm reference firmware used for protocol-recovery behavior | Codex |
| 2026-08-29 | Recorded TinyUSB ZLP and pyOCD packet-sizing evidence for the 508-byte wired DAP limit | Codex |
| 2026-08-29 | Recorded the pyOCD programming API used by automated Stage 1 HIL evidence | Codex |
