# AirDAP firmware

This directory contains the minimal ESP-IDF project for the ESP32-S3 used by
AirDAP. It currently builds the standard ESP-IDF second-stage bootloader, a
partition table, and a minimal factory application.

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

To flash and monitor a connected board:

```sh
idf.py -p <serial-port> flash monitor
```

The first-stage bootloader is stored in the ESP32-S3 mask ROM and is therefore
not part of this repository. Secure Boot, OTA partitions, hardware GPIO setup,
USB, and networking are intentionally deferred to their roadmap stages.
