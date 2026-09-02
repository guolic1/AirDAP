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

AirDAP keeps its ESP-IDF tools and Python environment in the ignored
`firmware/.airdap-env/` directory. Python 3.10 or newer must be available on
the host. The default setup also requires Git and network access the first
time it downloads the pinned ESP-IDF v6.1.0 source and its tools. The managed
checkout fetches only the ESP-IDF submodules used by AirDAP's standard and
debug-shell builds. Its tool installation is likewise limited to the
ESP32-S3 compiler, CMake, Ninja, and the ROM metadata used while configuring
the build; it does not download GDB, OpenOCD, clangd, or other target
toolchains.

On Linux, configure the environment once and activate it in the current Bash
shell:

```sh
cd firmware
python3 tools/setup.py
. ./get_env.sh
idf.py build
```

On Windows x64, use PowerShell:

```powershell
Set-Location firmware
python tools/setup.py
. .\get_env.ps1
idf.py build
```

If ESP-IDF v6.1.0 source is already present, pass its directory to avoid
downloading another copy. AirDAP still installs the same minimal ESP32-S3
build tools and Python packages under `firmware/.airdap-env/`, so later
activation does not depend on another ESP-IDF installation's tool state:

```sh
python3 tools/setup.py /path/to/esp-idf
```

```powershell
python tools/setup.py C:\path\to\esp-idf
```

Both setup modes save the selected source directory in
`firmware/.airdap-env/idf-path.txt`. In each new terminal, run only
`. ./get_env.sh` on Linux or `. .\get_env.ps1` in PowerShell before using the
normal `idf.py` command. The activation scripts must be sourced so they can
update the current shell. `setup.py` configures the environment only; it does
not build or flash the firmware. If a default download is interrupted while
fetching ESP-IDF submodules, run `setup.py` again to resume it.

The default managed checkout is intentionally scoped to this firmware. Its
missing ESP-IDF submodules are not fetched automatically during a build, so
unrelated ESP-IDF examples or features may not build against it. A path passed
to `setup.py` keeps ESP-IDF's normal submodule handling and can be used when a
full source checkout is needed. The setup script does not remove submodules or
tools downloaded by an older run; start with an empty `firmware/.airdap-env/`
to reclaim that space.

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

The build also verifies the fixed 8 MiB OTA layout and rollback configuration.
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
- a versioned NVS configuration store for provisioning metadata and bounded
  opaque credential slots;
- DAP/SWD ownership arbitration for USB, network, and internal diagnostics;
- a unified runtime mode state for USB presence, Wi-Fi, provisioning, OTA,
  and the current DAP owner;
- a Wi-Fi station manager with DHCP-gated online state and bounded reconnect
  backoff;
- a bounded transport-independent DAP service with session-safe response
  routing for USB and future network sessions;
- target reset, power/status GPIO, VTref, and USB VBUS monitoring.

The shared device identity is derived from the eFuse base MAC. Its USB serial
and current device ID are both `ADP-` followed by the full 12 uppercase MAC
digits. Its 128-bit UUID is the first 16 bytes of
`SHA-256("AirDAP" || eFuse base MAC)`. VID `0x303A` and PID `0x4021` are
development identifiers; product firmware must use identifiers the project is
authorized to ship. The checked-in layout is for the confirmed 8 MiB module
and provides two 4032 KiB OTA application slots. Secure Boot, Flash Encryption,
authenticated updates, authenticated network services, and BLE provisioning
remain deferred.

The firmware version is the single tag pointing directly at the built commit.
When that commit has no tag, the version is its seven-character Git hash. A
build fails if multiple tags point at the same commit or if Git metadata is
unavailable. The selected value is stored in ESP-IDF's application descriptor
and is reported consistently by OTA, CMSIS-DAP product firmware information,
and the debug shell. Use a clean build or `idf.py reconfigure` after adding or
removing a tag.

AirDAP frame protocol v1 uses a fixed 20-byte header with the fields `magic`,
`version`, `type`, `flags`, `session_id`, `sequence`, `payload_length`, and
`reserved` in that order. The magic bytes are `ADAP`; every multi-byte field is
in network byte order. The eight message types are `HELLO`, `AUTH`,
`DAP_REQUEST`, `DAP_RESPONSE`, `CONTROL_REQUEST`, `CONTROL_RESPONSE`,
`KEEPALIVE`, and `ERROR`. V1 requires `flags` and `reserved` to be zero, limits
all payloads to 4096 bytes, and further limits `DAP_REQUEST` to 508 bytes.

Session IDs and sequences reserve zero, start at one, increment by one, and
wrap from `UINT32_MAX` to one. Requests must arrive at the next expected
sequence; duplicates, stale values, and values ahead of the expected sequence
are distinct errors, using half-range serial-number comparison across wrap. A
response must repeat its request's session ID and
sequence. `DAP_REQUEST`/`CONTROL_REQUEST` map to their corresponding response
types; `HELLO`, `AUTH`, and `KEEPALIVE` use the same type in both directions;
`ERROR` may answer any request type.

Wire error codes are stable 16-bit network-order values: `0x0001` truncated,
`0x0002` payload too large, `0x0003` unsupported version, `0x0004` unsupported
type, `0x0005` invalid magic, `0x0006` invalid flags, `0x0007` invalid
reserved, `0x0010` session mismatch, `0x0011` duplicate sequence, `0x0012`
stale sequence, `0x0013` out-of-order sequence, `0x0014` response mismatch,
`0x0020` busy, `0x0021` unauthenticated, `0x0022` timeout, and `0x00FF`
internal. An `ERROR` payload is exactly one such 16-bit code. The frame decoder
distinguishes incomplete input from an invalid frame. A timed-out sequence
remains consumed: a retry uses the next sequence and the transport must discard
the late response. The component implements only framing and session/sequence
checks; it does not create a socket, authentication mechanism, timer, or DAP
worker.

CMSIS-DAP uses 512-byte internal buffers and advertises a 508-byte packet
limit. At full-speed USB this keeps the largest response from ending on an
exact 64-byte endpoint boundary, so TinyUSB cannot leave an automatic ZLP for
the following DAP transaction.

USB request framing remains in the USB component, while a shared DAP service
owns the four-entry worker queue and DAP execution task. Each queued request
captures its transport, service-issued session, response token, and callback.
The service validates the session before processing and again before response
delivery. Final validation and callback delivery are serialized with session
close, so close cannot return while an old response callback is still active
and a replacement USB session cannot receive that response. Queue-full,
timeout, stale-session, and response delivery failures remain observable
through service results and counters. Opening a NETWORK DAP service session
requires the caller to assert that its outer session was authenticated. This is
an admission contract, not an authentication implementation: the NETWORK
transport identifier remains internal at this stage and no network listener is
exposed.

The mode state publishes orthogonal USB, Wi-Fi, provisioning, OTA, and live DAP
owner fields. USB attach/detach, Wi-Fi station, and OTA lifecycle events are
wired today; provisioning remains `idle` until its later Phase 3 component
publishes events. USB DAP admission ignores Wi-Fi state. Authenticated NETWORK
DAP admission requires USB to be absent and Wi-Fi to be online, while USB
presence does not disable future network status, configuration, or OTA paths.
USB attach conditionally revokes an idle NETWORK DAP owner. Ownership acquire
and physical-operation begin revalidate versioned mode policy, so an attach
racing either transaction rolls it back without a blocking cross-task lock;
Wi-Fi-only changes are excluded from the USB policy version.
OTA receiving and committed states reject new USB, NETWORK, and DIAGNOSTIC DAP
owners. OTA entry also suspends the ownership arbiter until failure/abort or
reboot after commit. Running-image confirmation still follows only the required
local subsystem initialization and has no AP, DHCP, or internet dependency.

## Wi-Fi station manager

`config_store` remains the sole owner of global NVS initialization. The Wi-Fi
manager starts only after USB initialization and OTA running-image confirmation;
it never calls `nvs_flash_init()` or `nvs_flash_erase()`. Station credentials use
a versioned, length-delimited encoding in the existing Wi-Fi credential slot,
and the ESP-IDF Wi-Fi driver's credential storage is set to RAM so this slot is
the canonical persisted copy.

A station link is still reported as `connecting`. Only
`IP_EVENT_STA_GOT_IP`, after DHCP succeeds, publishes `online`. Authentication
and handshake failures are reported separately from AP loss and other temporary
disconnects. Losing the station IP immediately returns the state to `connecting`
until DHCP recovers. Both disconnect classes use application-managed
exponential retry delays starting at 1 second and capped at 60 seconds. A
committed credential update cancels any pending retry, resets the delay,
disconnects the previous attempt if needed, and applies the latest committed
credentials immediately. SSIDs and passwords are never logged.

The project pins `espressif/network_provisioning` 1.2.4 for the later BLE
provisioning slice and explicitly enables protocomm Security 2. BLE transport,
PoP injection, provisioning-window behavior, and custom pairing endpoints are
not part of the station-manager slice.

For development-board Wi-Fi validation, use a dedicated test AP and never a
production credential. The HIL console accepts credentials at runtime so they
do not enter Git, build metadata, or command-line history:

```sh
cd firmware/test/hil/wifi_manager
idf.py set-target esp32s3
idf.py -p <airdap-programming-port> flash monitor
```

Run `set`, provide a deliberately wrong password, and confirm an
`authentication failed` log followed by increasing retry intervals. Run `set`
again with the correct password; `status` must become `online` without waiting
for the old retry. Power off the test AP and confirm a `temporarily disconnected`
log and `disconnected` status, then restore it and confirm automatic recovery to
`online`. Run `clear` afterward. This workflow flashes a HIL application and
therefore requires explicit authorization for the selected board. It replaces
the standard AirDAP partition table/application until the normal firmware is
flashed again. A host test or firmware build does not replace these RF,
association, and DHCP observations.

`DAP_Connect` acquires its transport's DAP/SWD owner. USB may preempt an idle
NETWORK owner after attach; it does not tear down an in-flight operation or a
DIAGNOSTIC owner. Other owner conflicts fail without driving that owner's bus.
Disconnect, USB detach, stale USB sessions, and OTA write entry release
ownership; release also leaves SWDIO high impedance and nRESET deasserted.
Every new owner starts with an SWD Line Reset. The Debug Shell `swd-idcode`
command acquires the internal DIAGNOSTIC owner for its complete transaction and
reports `busy` when USB or NETWORK already owns SWD. NETWORK remains reserved
for a later network transport and is not exposed by this stage.

## Persistent configuration

Configuration schema 1 stores the friendly name, provisioning state, and
bounded opaque slots for Wi-Fi credentials, pairing records, and network
authentication material. The component writes one versioned NVS blob and only
publishes a changed in-memory snapshot after both `nvs_set_blob()` and
`nvs_commit()` succeed. A component-owned mutex serializes public readers and
writers across capture, commit, and snapshot publication. Selective clear
operations rewrite the same record and leave fields outside the requested scope
unchanged. Clearing any network slot also returns the persistent provisioning
state to `unprovisioned`.

Only `ESP_ERR_NVS_NO_FREE_PAGES` and `ESP_ERR_NVS_NEW_VERSION_FOUND` trigger a
controlled erase and one initialization retry. An incompatible AirDAP schema,
invalid record, open/read/write failure, or commit failure remains observable
and does not silently reset configuration.

Configuration handles use ESP-IDF's `NVS_READWRITE_PURGE` mode so replaced or
cleared values are purged rather than left as deleted NVS entries. The current
development profile does not enable Flash Encryption, so current values remain
plaintext at rest and this store is not a product credential-security
boundary. Do not use production credentials until the product key lifecycle,
Flash Encryption, and provisioning process are approved and verified.

## Development USB OTA

The first rollout from the former single-`factory` layout requires one complete
serial flash. This installs the 8 MiB flash header, bootloader rollback
support, partition table, initial OTA metadata, and the application in
`ota_0`:

```sh
idf.py -p <airdap-programming-port> flash
```

An application-only update cannot migrate an existing device's partition
table. Do not use the Python updater until this baseline flash has completed
successfully.

After the baseline is installed, build the next application and update over
the normal AirDAP USB connection without pressing reset or GPIO0. On Windows,
sync the checked-in `uv` environment and run the updater from the repository
root:

```powershell
uv sync
uv run python firmware/tools/airdap-update.py --serial ADP-001122334455 firmware/build/airdap.bin
```

In an existing non-Windows Python environment, install PyUSB and run the tool
from `firmware/`:

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

On Windows, use the checked-in `uv` environment from the repository root to
open the interface with the checked-in host tool:

```powershell
uv sync
uv run python firmware/tools/airdap-shell.py --serial ADP-001122334455
```

In an existing non-Windows Python environment, install PyUSB and run the tool
from `firmware/`:

```sh
python -m pip install pyusb
python tools/airdap-shell.py --serial ADP-001122334455
```

When only one AirDAP is connected, `--serial` may be omitted. Press Ctrl-] or
Ctrl-D to leave the local tool; Ctrl-C is forwarded to cancel the current
firmware input line. In interactive mode, Tab completes a unique command name,
or lists all matching commands when the prefix is empty or ambiguous. Left/Right,
Home/End, and Delete edit the current line, and Up/Down browses the eight most
recent commands from the current session. When the input line is not empty,
history navigation only visits commands with that prefix. Moving down past the
newest matching entry restores the unsubmitted input line. Commands can also be
run non-interactively:

```sh
python tools/airdap-shell.py \
    -c help -c identity -c config-status -c status -c "wifi status" \
    -c "swd-idcode 100"
```

`wifi set` is interactive-only so credentials cannot be supplied through shell
command history or `-c` process arguments. It first prompts for an SSID and then
for a password; password input is not echoed, is not added to shell history,
and is scrubbed from the input/session buffers after submission or Ctrl-C.
SSID input accepts 1–32 printable ASCII bytes and password input accepts 0–64;
an empty password selects an open network. Use `wifi status` afterward: only a
DHCP-confirmed connection reports `wifi=online`.

`--color auto` is the default: it enables ANSI colors for an interactive TTY
and keeps `-c` command output plain for scripts. Use `--color always` to force
colors or `--color never` to disable them. Cyan marks the prompt and command
names, green marks successful diagnostic results, yellow marks usage guidance
and restart acknowledgement, and red marks errors. Mirrored application logs
retain their original bytes and coloring.

`restart` may also be used with `-c`, but it must be the final command because
the device disconnects after its acknowledgement is delivered.

The host tool makes Vendor Bulk communication behave like a raw text terminal.
The firmware accepts printable ASCII, CR/LF line endings, backspace/delete,
Ctrl-C, Tab, and ANSI navigation sequences. Available commands are:

- `help` — list commands;
- `identity` — print the USB serial, device ID, UUID, firmware and protocol
  versions, and capability bits from the shared device identity;
- `config-status` — print only the configuration schema and provisioning state;
  credential values are never included;
- `status` — print `target_mv`, `usb_vbus_mv`, `uptime_ms`, and `free_heap`;
- `wifi status` — print `wifi=stopped`, `disconnected`, `connecting`, or
  `online` without displaying credentials;
- `wifi set` — interactively replace the stored SSID and password, reset
  reconnect backoff, and reconnect immediately;
- `wifi clear` — remove stored Wi-Fi credentials and stop reconnect attempts;
- `swd-idcode [clock_khz]` — reset the SWD line, select SWD, and read the
  target DP IDCODE at 100 kHz by default; accepted clocks are 100–10,000 kHz;
- `restart` — wait for the acknowledgement transfer to complete, then restart
  AirDAP; a bounded transfer timeout leaves the firmware running.

When OpenOCD or another CMSIS-DAP client owns SWD, `swd-idcode` returns `busy`
without driving the target. After every successful, failed, or USB-detached
attempt, the command releases its DIAGNOSTIC owner, leaves SWDIO high impedance,
and deasserts nRESET.

After `airdap-shell` starts a session, normal ESP application logs are mirrored
to the Vendor Bulk interface and continue to use the configured primary
console. When a log arrives during editing, the shell restores the prompt,
current input, and cursor after printing it. Mirroring uses a bounded queue and
may drop burst logs instead of blocking application tasks. ROM, bootloader,
early application messages, and direct standard output are not captured by the
Vendor interface. The shell is intended for physically connected development
systems: it has no authentication, and anyone with access to it can replace or
clear persistent Wi-Fi credentials. The current development profile does not
enable Flash Encryption, so those credentials remain plaintext at rest. The
shell provides only bounded diagnostics and Wi-Fi credential management; it
deliberately provides no arbitrary DP/AP access, target memory access,
programming, persistent history, or dynamic log-control commands.

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
    bootloader_artifact ota_layout setup_env board config_store device_identity voltage_monitor swd_protocol \
    dap_ownership mode_state dap_backend dap_protocol dap_service airdap_frame \
    dap_ota dap_stream ota_manager app_main wifi_manager target_uart usb_descriptors project_version \
    debug_shell_config_status debug_shell_identity debug_shell_input debug_shell_wifi \
    debug_shell_swd_probe debug_shell_tx_state airdap_shell airdap_update wired_hil; do
    cmake -S "test/unit/$suite" -B "build-host/$suite"
    cmake --build "build-host/$suite"
    ctest --test-dir "build-host/$suite" --output-on-failure
done
```

These tests prove GPIO ordering, bootloader artifact-contract validation,
versioned configuration validation, commit-before-publish behavior, serialized
concurrent writes, fake-NVS restart recovery, selective configuration clearing,
safe configuration-status command behavior, ADC scaling, SWD transaction
framing, Wi-Fi credential encoding, wrong-password classification, DHCP-gated
online state, IP-loss handling, bounded reconnect backoff, actual timer/driver
coordination, configuration-change event ordering and recovery, DAP owner
transitions and physical-backend release calls, unified
USB/Wi-Fi/provisioning/OTA mode transitions and DAP admission, CMSIS-DAP and OTA
command framing, OTA state transitions, stale USB-frame recovery, interleaved
USB/NETWORK DAP routing, AirDAP frame golden vectors and sequence rules,
stale-session response suppression, bounded queue failures, host update
ordering, UART line-coding
mapping, both compile-time USB descriptor variants, bounded shell input, the
bounded SWD IDCODE command flow, debug TX completion state, host tools, and
wired HIL helper's protocol checks. They do not prove USB enumeration, real NVS
power-loss persistence or purge behavior, physical OTA persistence, bootloader
rollback on a board, or electrical SWD timing.
Follow `test/hil/wired.md` on a populated AirDAP board before marking roadmap
Stage 1 complete.
