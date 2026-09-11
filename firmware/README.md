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
`firmware/.airdap-env/` directory. Python 3.13 or newer must be available on
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
uv run --locked python tools/setup.py
. .\get_env.ps1
idf.py build
```

PowerShell activation prefers `uv` and uses the repository's locked Python
environment. If `uv` is unavailable, `get_env.ps1` falls back to the `python`
command already available on `PATH`.

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

Both setup modes save the selected source directory and its managed/external
provenance in `firmware/.airdap-env/idf-path.txt`. Existing one-line path files
remain supported. In each new terminal, run only `. ./get_env.sh` on Linux or
`. .\get_env.ps1` in PowerShell before using the normal `idf.py` command. The
activation scripts must be sourced so they can update the current shell.
`setup.py` configures the environment only; it does not build or flash the
firmware. If a default download is interrupted while fetching ESP-IDF
submodules, run `setup.py` again to resume it.

The default managed checkout is intentionally scoped to this firmware. Its
missing ESP-IDF submodules are not fetched automatically during a build, so
unrelated ESP-IDF examples or features may not build against it. A path passed
to `setup.py` keeps ESP-IDF's normal submodule handling and can be used when a
full source checkout is needed. The setup script does not remove submodules or
tools downloaded by an older run; start with an empty `firmware/.airdap-env/`
to reclaim that space.

The default build enables the debug shell while preserving the existing
CMSIS-DAP plus target-UART CDC layout. A normal `idf.py build` appends the
additional Vendor Bulk interface; no alternate SDKConfig profile is required.
The same selection is available in `idf.py menuconfig`. CMake stops with an
error if the AirDAP option is enabled while TinyUSB has fewer than two Vendor
interfaces. The CDC count remains one.

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
- CDC ACM bridging through the shared target UART1 service on GPIO17/GPIO18;
- an optional, independent Vendor Bulk debug shell;
- an SPI2 half-duplex SWD backend on GPIO12/GPIO13/GPIO14;
- a versioned NVS configuration store for provisioning metadata and bounded
  opaque credential slots;
- DAP/SWD ownership arbitration for USB, network, and internal diagnostics;
- a unified runtime mode state for USB presence, Wi-Fi, provisioning, OTA,
  and the current DAP owner;
- a Wi-Fi station manager with DHCP-gated online state and bounded reconnect
  backoff;
- mDNS discovery on the station interface after DHCP succeeds;
- an on-demand BLE provisioning window using protocomm Security 2 and
  a public credential gated by physical button access;
- a TLS 1.3 PSK-DHE authentication layer with one rotatable credential and
  one authenticated network-owner session;
- a bounded authenticated DAP listener on TCP 3260 with AirDAP v1 framing,
  strict session/sequence checks, and disconnect-safe response routing;
- a bounded transport-independent DAP service with session-safe response
  routing for USB and authenticated network sessions;
- target reset, power/status GPIO, VTref, and USB VBUS monitoring.

The shared device identity is derived from the eFuse base MAC. Its USB serial
and current device ID are both `ADP-` followed by the full 12 uppercase MAC
digits. Its 128-bit UUID is the first 16 bytes of
`SHA-256("AirDAP" || eFuse base MAC)`. VID `0x303A` and PID `0x4021` are
development identifiers; product firmware must use identifiers the project is
authorized to ship. The checked-in layout is for the confirmed 8 MiB module
and provides two 4032 KiB OTA application slots. Secure Boot, Flash Encryption,
authenticated updates, authenticated UART/control listeners, and a production
credential lifecycle remain deferred.

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
an admission contract; the TLS 1.3 PSK listener on TCP 3260 performs the outer
authentication before opening the NETWORK service session.

The mode state publishes orthogonal USB, Wi-Fi, provisioning, OTA, and live DAP
owner fields. USB attach/detach, Wi-Fi station, BLE provisioning, and OTA
lifecycle events are wired today. USB DAP admission ignores Wi-Fi state.
Authenticated NETWORK DAP admission requires USB to be absent and Wi-Fi to be
online, while USB presence does not disable independent network status,
configuration, or OTA paths.
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
and `CONFIG_ESP_WIFI_NVS_ENABLED` is disabled. Driver storage is also selected
as RAM for normal operation, so neither ordinary reconnects nor the upstream
provisioning manager can persist a second copy; this slot remains the canonical
persisted value. The ten-second network reset also purges ESP-IDF's Wi-Fi NVS
namespace so credentials left by older firmware cannot survive the reset. This
does not affect AirDAP's compiled Security 2 credential.

A station link is still reported as `connecting`. Only
`IP_EVENT_STA_GOT_IP`, after DHCP succeeds, publishes `online`. Authentication
and handshake failures are reported separately from AP loss and other temporary
disconnects. Losing the station IP immediately returns the state to `connecting`
until DHCP recovers. Both disconnect classes use application-managed
exponential retry delays starting at 1 second and capped at 60 seconds. A
committed credential update cancels any pending retry, resets the delay,
disconnects the previous attempt if needed, and applies the latest committed
credentials immediately. SSIDs and passwords are never logged.

The project pins `espressif/network_provisioning` 1.2.4 and enables BLE plus
protocomm Security 2. While a window is active, the upstream manager exclusively
controls Wi-Fi association; AirDAP's reconnect state machine resumes only after
the provisioning manager ends and then reapplies the canonical configuration.
A failed, cancelled, or timed-out attempt restores the previously committed
configuration. The custom network-credential pairing endpoint is available
during the same Security 2 window. The authenticated DAP listener uses the
committed network credential on TCP 3260; UART and control TCP transports remain
later work.

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

## mDNS discovery

After the Wi-Fi station obtains an IP address, AirDAP publishes one
`_airdap._tcp.local` service on TCP port 3260. The stable hostname is
`airdap-<12 lowercase MAC digits>.local`, and the service instance is the
shared `ADP-<12 uppercase MAC digits>` device ID. The TXT record contains:

```text
id=ADP-001122334455
proto=1
fw=<exact tag or seven-character Git hash>
cap=swd,uart,power,reset,ota
state=idle
dap_port=3260
uart_port=3261
```

The identity, protocol version, firmware version, and capability selection all
come from `device_identity`; discovery does not derive a second identity. A
repeated `IP_EVENT_STA_GOT_IP`, including an address change, removes and adds
the service again so the current address is announced. `IP_EVENT_STA_LOST_IP`
removes the service locally. Host caches may retain an expired address for its
TTL and are not evidence that the device is still advertising it.

mDNS fields are unauthenticated hints. They contain no Wi-Fi or pairing secret
and must never be used to authorize a connection. TCP ports 3260 and 3261 are
the reserved AirDAP protocol ports. TCP 3260 now provides authenticated DAP;
control operations and the target-UART listener remain deferred to their
network-transport slices.

Follow [`test/hil/mdns.md`](test/hil/mdns.md) to validate live announcements,
address replacement, offline withdrawal, and firmware-version consistency from
Linux and Windows. This RF and LAN boundary cannot be established by the host
unit test or firmware build.

## BLE Security 2 provisioning

BLE is disabled during normal operation. Hold `BOOT_KEY` (GPIO0) until the
green network LED turns on at three seconds, then release it to open a
120-second provisioning window. The button never starts BLE while it remains
pressed. Repeating the same hold-and-release while the window is active cancels
the attempt. The BLE service name is the shared
`ADP-<12 uppercase MAC digits>` device ID. A successful Wi-Fi/DHCP check
atomically commits the new credentials and marks the device provisioned.
The service remains available long enough for the client to query that success,
then the upstream 30-second auto-stop ends BLE and releases its resources.
Failure leaves the window open for another client attempt; cancel and timeout
stop BLE and restore the previously committed Wi-Fi configuration.

Continue holding `BOOT_KEY` past the green indication until the red status LED
turns on at ten seconds. No provisioning or clear action runs while the button
remains pressed. Release it to turn both indicators off, clear Wi-Fi
credentials plus the reserved pairing and network-authentication slots, and
restart without intentionally entering ROM download mode. After restart the
device can open a fresh provisioning window using the same public Security 2
credential.

AirDAP intentionally uses Espressif's public Security 2 development credential:

- username: `wifiprov`
- PoP/password: `abcd1234`

Its salt and SRP verifier are compiled into the application, so a normal build
and flash require no credential-generation or injection step:

```sh
cd firmware
idf.py build
idf.py -p <airdap-programming-port> flash
```

From the repository root, sync the checked-in host environment and run the
AirDAP provisioning tool:

```powershell
uv sync
uv run python firmware/tools/airdap-provision.py
```

In an existing non-Windows Python environment, install the host dependencies
and run the tool from `firmware/`:

```sh
python -m pip install bleak protobuf cryptography
python tools/airdap-provision.py
```

The tool reads the ESP-IDF path saved by `tools/setup.py`, scans only active
`ADP-...` provisioning services, and supplies the firmware's public Security 2
credential automatically. A single AirDAP is selected automatically; multiple
AirDAP devices produce an explicit selection prompt. The upstream client then
prompts for the Wi-Fi access point and reads its passphrase without echo. The
passphrase is not accepted on the command line or saved by this host tool.

The tool uses `airdap_esp_prov.py` to connect directly to the BLE provisioning
service without OS pairing or unpairing. AirDAP's GATT endpoints allow this;
Security 2 still protects the provisioning messages. This avoids a Windows
device-association wait of about 60 seconds and leaves existing OS bonds alone.
The original Espressif client only requests pairing on Windows, so that
particular delay does not affect its Linux path. The AirDAP transport also
passes the discovered BLEDevice to Bleak to avoid an additional address scan.

The pinned `espressif/network_provisioning` host client must already be present
in `managed_components/`; if it has not yet been downloaded, activate the
configured ESP-IDF environment and run `idf.py reconfigure` once.

Security 2 still encrypts and authenticates the BLE provisioning session, but
the published PoP does not identify an owner. Physical access to hold
`BOOT_KEY` through the three-second indication and release it is therefore the
only provisioning authorization boundary. Any nearby party that knows the
public credential can race or replace Wi-Fi configuration while that window is
open. This design is appropriate only where physical access to the button is
trusted; it is not per-device authentication. Follow
[`test/hil/ble_provisioning.md`](test/hil/ble_provisioning.md)
before relying on BLE lifecycle, RF behavior, Wi-Fi association, persistence,
or the GPIO0 reset guard on hardware.

## Network pairing and authenticated sessions

AirDAP keeps exactly one active 256-bit network PSK. Its stable, non-secret TLS
identity is `AIRDAP:<device_id>`. While the physical-button BLE window is open,
the `airdap-pair` Security 2 endpoint accepts version `1` plus a 32-byte PSK.
The device publishes the new generation only after the NVS commit succeeds and
returns only `SHA-256("AirDAP network PSK v1" || identity || PSK)`. Repeating
the same candidate is idempotent. Rotating to a different key revokes the old
authenticated session; the old host must pair again before it can reconnect.

The host tool has no implicit credential location. `--credential` is required;
if that explicit path does not exist, the tool generates the key with the host
CSPRNG and creates the file exclusively. The file contains the development PSK
in plaintext: never commit it, and keep it in a protected user directory. POSIX
loads reject group/other permission bits and new files use mode `0600`; on
Windows, protect the selected directory with an appropriate user-only ACL.

From the repository root on Windows, open the BLE window and run:

```powershell
uv sync --locked
uv run python firmware/tools/airdap-pair.py `
    --device ADP-001122334455 `
    --credential "$env:LOCALAPPDATA\AirDAP\ADP-001122334455.json"
```

From `firmware/` in an existing Linux Python 3.13 or newer environment:

```sh
python tools/airdap-pair.py \
    --device ADP-001122334455 \
    --credential "${XDG_CONFIG_HOME:-$HOME/.config}/airdap/ADP-001122334455.json"
```

The authentication component completes the blocking TLS handshake before any
application data is read. It accepts only TLS 1.3 PSK-DHE with
`TLS_AES_128_GCM_SHA256`; pure PSK, unauthenticated ephemeral TLS, TLS 1.2,
0-RTT, and server session tickets are disabled. At most two handshakes may be
pending, each with a five-second timeout. A TLS connection becomes an AirDAP
session only after `AUTH` binds it. The first binding creates the sole owner and
a random 32-byte session token; later DAP, UART, or OTA connections may join
only with that token. Privileged dispatch must revalidate the binding, and a
disconnect, 60-second idle timeout, network clear, credential rotation, or
restart releases it.

The standard-library Python TLS-PSK probe verifies only the negotiated version
and cipher:

```sh
python tools/airdap-tls-probe.py 192.0.2.10 \
    --credential "${XDG_CONFIG_HOME:-$HOME/.config}/airdap/ADP-001122334455.json"
```

The DAP listener starts only after the Wi-Fi manager starts successfully, and
mDNS is published only after that listener is ready. Each connection completes
TLS before reading AirDAP data and then requires `HELLO` followed by `AUTH`.
The frame `session_id` is a non-zero client-selected connection ID with sequence
numbers starting at one; the logical authenticated owner session remains a
separate value returned by `AUTH`.

V1 uses these payloads on TCP 3260:

- `HELLO` request is empty. Its response is the 16-byte device UUID, a 32-bit
  big-endian capability mask, the fixed 16-byte `ADP-...` device ID, and the
  remaining UTF-8 bytes as the firmware version.
- `AUTH` request is empty for the first owner, or carries the existing 32-byte
  owner token when a later service joins. Its response is the 32-bit big-endian
  logical owner session ID followed by the 32-byte random owner token.
- `DAP_REQUEST` and `DAP_RESPONSE` carry raw CMSIS-DAP packets. Requests remain
  limited to 508 bytes and each DAP connection permits one in-flight request.
  The USB-development OTA vendor commands `0x80` through `0x85` are rejected;
  authenticated network OTA remains deferred to its dedicated control slice.
- `KEEPALIVE` has an empty request and response and refreshes the authenticated
  owner idle deadline.

The response callback only enters a bounded queue; the network connection task
writes TLS responses after leaving the DAP service critical section. DAP work
uses the existing one-second service deadline. A timeout consumes its AirDAP
sequence and suppresses a late response. Authentication expiry, credential
rotation, network clear, or disconnect shuts down the matching registered
socket, closes the NETWORK DAP service session, and releases ownership before
the slot can be reused.

Use the authenticated probe to verify `HELLO`, `AUTH`, product firmware
`DAP_Info`, and `KEEPALIVE` without printing the returned session token:

```sh
python tools/airdap-dap-probe.py 192.0.2.10 \
    --credential "${XDG_CONFIG_HOME:-$HOME/.config}/airdap/ADP-001122334455.json"
```

ESP-IDF 6.1 currently supplies Mbed TLS 4.1.0. Until a compatible ESP-IDF
update supplies Mbed TLS 4.1.1 or newer, AirDAP keeps server tickets disabled
and requires the explicit handshake-before-data boundary described above.

`DAP_Connect` acquires its transport's DAP/SWD owner. USB may preempt an idle
NETWORK owner after attach; it does not tear down an in-flight operation or a
DIAGNOSTIC owner. Other owner conflicts fail without driving that owner's bus.
Disconnect, USB detach, stale USB sessions, and OTA write entry release
ownership; release also leaves SWDIO high impedance and nRESET deasserted.
Every new owner starts with an SWD Line Reset. The Debug Shell `swd-idcode`
command acquires the internal DIAGNOSTIC owner for its complete transaction and
reports `busy` when USB or NETWORK already owns SWD. NETWORK is used by the
authenticated DAP listener on TCP 3260; its connection and DAP-session lifecycle
is observable through the read-only Debug Shell diagnostics below.

## Authenticated target control

TCP 3260 accepts reset/power `CONTROL_REQUEST` frames after TLS, HELLO and
AUTH. It revalidates the exact logical owner before each board operation;
expired, revoked, stale and unbound sessions cannot access board I/O. Frame
session/sequence validation and replay rejection are shared with DAP.

The shared opcode allocation is in `airdap_control.h`: UART uses `0x10`–`0x14`
on TCP 3261 (P5-T2), while TCP 3260 accepts:

| Operation | Request payload | CONTROL_RESPONSE payload |
|---|---|---|
| RESET_SET | `20 asserted` | `20 asserted` |
| POWER_SET | `21 allowed` | `21 allowed` |
| POWER_GET | `22` | `22 active` |

Each field shown is one byte; booleans must be exactly `00` or `01`. Successful
responses echo the request's frame session and sequence. An ERROR response
contains exactly one big-endian uint16 error code, without an opcode prefix:
short payload `0001`, unknown opcode `0004`, busy/disallowed target state
`0020`, unauthenticated `0021`, invalid value/trailing bytes `0023`, and board
or internal failure `00ff`. Errors consume the accepted request sequence.

USB presence, offline Wi-Fi, non-idle OTA, USB/DIAGNOSTIC DAP ownership and
in-flight physical DAP work block control. POWER_SET also requires no DAP
owner: issue DAP_Disconnect before changing power permission. Control uses a
short ownership reservation without acquiring DAP or emitting SWD Line Reset.
The reservation ends before any TLS output, including on board failure.

RESET_SET latches the commanded state; send `20 00` to release reset explicitly.
Power permission also remains until changed or board initialization. Closing
a control-only connection does not itself change GPIOs; the existing DAP-owner
release still deasserts reset. DAP SWJ_Pins reads the same last successful reset
command as the board API, not a private transport cache or physical NRST voltage.

POWER_SET `01` only releases GPIO9's open-drain output; `00` pulls it low.
POWER_GET samples the shared status net without releasing or otherwise changing
it. Neither a SET acknowledgement nor the sampled bit proves USB selection,
VTref, full power-cycle sequencing or electrical safety under load. PCB power
and reset electrical acceptance still requires instruments and the wired HIL
procedure.

## Target UART service

UART1 on GPIO17/GPIO18 has one physical RX worker and one active session slot
per USB or NETWORK transport. Each live session receives an ordered copy in
its own fixed 512-byte ring. A full ring drops only that session's newest
bytes, increments its saturating drop counter, and never waits for the slow
consumer while serving the other session.

UART configuration and writes use a first-come, non-preemptive owner tied to
the exact transport session ID. A competing ownership request receives `busy`;
closing the current owner releases it, while a stale close cannot release a
replacement session. Reads are independent of TX ownership. The USB CDC bridge
opens and closes its service session with the CDC lifecycle, caches the current
line coding, and does not consume service RX bytes while the TinyUSB IN queue
has no space.

USB CDC remains an unauthenticated physical development interface. The shared
service rejects unauthenticated NETWORK session creation, but TCP 3261,
network parameter negotiation, and per-operation authentication revalidation
belong to P5-T2 and are not implemented here.

## Persistent configuration

Configuration schema 1 stores the friendly name, provisioning state, and
bounded opaque slots for Wi-Fi credentials, pairing records, and network
authentication material. The network-authentication slot holds one internal
versioned PSK record and no session token. The component writes one versioned
NVS blob and only
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
a software restart, waits for the old USB enumeration to disappear, then waits
for the same USB serial and reports its running version. It writes only
`airdap.bin`; it does not replace the bootloader, partition table, NVS, PHY data,
or OTA metadata partition directly.

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

## Vendor Bulk debug shell

The default build preserves the existing USB assignments and appends the shell:

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
    -c system-info -c memory-info -c mode-status -c ota-status \
    -c target-status -c tasks -c dap-stats -c network-info \
    -c network-status -c sessions -c usb-status -c uart-status \
    -c discovery-status
```

Use `--sleep SECONDS` to wait between each adjacent pair of repeated `-c`
commands. The delay begins only after the preceding command has completed and
the firmware prompt has returned; no delay is added after the final command.

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
Ctrl-C, Tab, and ANSI navigation sequences. `help` lists every registered
command with a one-line summary; `help <command>` prints its usage and full
description. Command descriptors are registered during shell startup, so a
command group can be defined and registered from its own source file without
extending a single global command table. Available commands are:

- `help [command]` — list commands or show detailed help for one command;
- `system-info` — print the USB serial, device ID, UUID, firmware and protocol
  versions, capability bits, ESP-IDF version, uptime, chip model, revision,
  core count, feature bits, and the previous reset reason;
- `memory-info` — print total, free, historical minimum-free, and largest-free
  block sizes for default, internal, DMA, and SPI RAM heap capabilities; these
  capability categories can overlap and should not be summed;
- `mode-status` — print the safe persistent configuration schema and provisioned
  state, followed by USB, Wi-Fi, provisioning, OTA, and DAP-owner state from the
  shared runtime snapshot; credential values are never included;
- `ota-status` — print running version, OTA protocol/session/rollback state,
  running image state, and running/boot partition metadata without starting an
  update;
- `target-status` — read target power-active status, target/USB voltage, and
  DAP ownership without changing target pin direction or level;
- `tasks [--interval <ms>]` — take a bounded FreeRTOS task snapshot and print
  task number, name, state, core affinity, current/base priorities, stack
  high-water mark in free bytes, run time, and dual-core-normalized CPU
  percentage, sorted by run time;
  values are cumulative without arguments, while a 100–5000 ms interval reports
  deltas over that sampling window;
- `dap-stats` — print DAP service request, response, queue saturation, timeout,
  stale-work, and delivery-failure counters without resetting them;
- `network-info` — print Wi-Fi manager/link/configuration state, failure and
  retry diagnostics, IPv4 address information, RSSI, and channel without
  displaying SSID, BSSID, credentials, or authentication material;
- `network-status` — aggregate Wi-Fi online state, IPv4 address, RSSI, mDNS
  publication, authenticated DAP-listener readiness, and the current DAP owner
  without changing any network or ownership state;
- `sessions` — print credential presence, pending TLS handshakes, logical owner
  activity, bound authentication connections, and the separately counted
  allocated, TLS, authenticated, and open DAP connections without displaying
  credentials, fingerprints, keys, or session tokens;
- `usb-status` — print TinyUSB mount/suspend state, DAP Vendor, target CDC and
  debug Vendor interface state, and whether the USB DAP session is active;
- `uart-status` — print the target UART's last accepted line coding, RX queued
  bytes, TX free space, cumulative transferred bytes, and driver I/O failures
  without consuming UART data;
- `discovery-status` — print mDNS initialization, lifecycle/publication state,
  advertised hostname and service ports, and the latest lifecycle error without
  publishing or withdrawing the service;
- `wifi status` — print `wifi=stopped`, `disconnected`, `connecting`, or
  `online` without displaying credentials;
- `wifi set` — interactively replace the stored SSID and password, reset
  reconnect backoff, and reconnect immediately;
- `wifi clear` — remove stored Wi-Fi credentials and stop reconnect attempts;
- `button press|release|status` — set or inspect a RAM-only simulated
  `BOOT_KEY`; physical and simulated presses pass through the same 3-second,
  10-second, and release-confirmation behavior;
- `swd-idcode [clock_khz]` — reset the SWD line, select SWD, and read the
  target DP IDCODE at 100 kHz by default; accepted clocks are 100–10,000 kHz;
- `restart` — wait for the acknowledgement transfer to complete, then restart
  AirDAP; a bounded transfer timeout leaves the firmware running.

For HIL automation, one host invocation can hold the simulated button across
the normal device-side threshold before releasing it:

```powershell
uv run python firmware/tools/airdap-shell.py `
    -c "button press" --sleep 3.5 -c "button release"
uv run python firmware/tools/airdap-shell.py -c "button status"
```

Without `--interval`, the `tasks` CPU values are cumulative since boot. With an
interval, the command takes two snapshots and reports the difference; scheduling
runs normally between snapshots. Tasks created during the interval have a zero
delta, and tasks deleted before the second snapshot are omitted. On this two-core
target, percentages use the combined capacity of both cores, so the rows normally
total close to 100%. The `affinity` column is the allowed core, not necessarily
the core on which an unpinned task was executing at capture time. Each snapshot
pauses scheduling on both cores while it copies task metadata and scans stack
high-water marks, and is capped at 48 tasks. This debug-only operation can cause
a perceptible scheduling and USB latency spike, especially with many or large
task stacks. The standard configuration enables per-task run-time accounting
and `sdkconfig.defaults` selects a 64-bit, 1 MHz ESP Timer counter, so values
use the `runtime_us` or `runtime_delta_us` suffix and long-running debug
sessions do not quickly wrap it. A manually selected alternative clock uses
`runtime_ticks` or `runtime_delta_ticks` instead.

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
clear persistent Wi-Fi credentials or simulate the physical provisioning/reset
button. A simulated press remains active across shell sessions until
`button release` or a device restart. The current development profile does not
enable Flash Encryption, so those credentials remain plaintext at rest. The
shell provides only bounded diagnostics, Wi-Fi credential management, and
BOOT_KEY simulation; it deliberately provides no arbitrary DP/AP access,
target memory access,
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
    dap_ownership mode_state dap_backend dap_protocol dap_service airdap_frame discovery \
    dap_ota dap_stream ota_manager app_main wifi_manager ble_provisioning network_auth network_dap network_control \
    target_uart usb_uart_bridge usb_descriptors project_version \
    debug_shell_commands debug_shell_diagnostics debug_shell_config_status \
    debug_shell_identity debug_shell_input debug_shell_wifi debug_shell_button \
    debug_shell_swd_probe \
    debug_shell_tx_state airdap_shell airdap_update \
    airdap_provision airdap_pair airdap_tls_probe airdap_dap_probe wired_hil; do
    cmake -S "test/unit/$suite" -B "build-host/$suite"
    cmake --build "build-host/$suite"
    ctest --test-dir "build-host/$suite" --output-on-failure
done
```

These tests prove GPIO ordering, bootloader artifact-contract validation,
versioned configuration validation, commit-before-publish behavior, serialized
concurrent writes, fake-NVS restart recovery, selective configuration clearing,
safe persistent-configuration formatting and `mode-status` reporting, ADC scaling, SWD transaction
framing, Wi-Fi credential encoding, wrong-password classification, DHCP-gated
online state, IP-loss handling, bounded reconnect backoff, actual timer/driver
coordination, configuration-change event ordering and recovery, Security 2
public-credential derivation against its published username and PoP,
provisioning-button thresholds, BLE window cleanup, atomic provisioning commit,
network-credential commit/rotation, exact TLS 1.3 PSK-DHE configuration,
bounded handshake admission and cleanup, fingerprint vectors, authenticated
session binding, single-owner/token admission, expiry/replay/revocation, the
non-secret authentication and DAP-listener status snapshots, their concurrent
getter behavior, aggregate network/session Debug Shell output, the
Python pairing/TLS-PSK/DAP probe tools, authenticated AirDAP HELLO/AUTH framing,
DAP dispatch/timeout response routing, revoke/disconnect cleanup, authenticated
reset/power framing, fake-board dispatch and error propagation, control/DAP
operation exclusion, shared commanded reset state, and
reset-after-release behavior, DAP owner
transitions and physical-backend release calls, unified
USB/Wi-Fi/provisioning/OTA mode transitions and DAP admission, CMSIS-DAP and OTA
command framing, mDNS identity/TXT formatting and IP-driven publish/refresh/
withdraw behavior, OTA state transitions, stale USB-frame recovery, interleaved
USB/NETWORK DAP routing, AirDAP frame golden vectors and sequence rules,
stale-session response suppression, bounded queue failures, host update
ordering, automatic AirDAP BLE discovery and public-credential command
construction, UART line-coding mapping, independent bounded UART RX fan-out
and overflow accounting, exact-session TX ownership and USB CDC session
cleanup, both compile-time USB descriptor variants, bounded shell input, the
simulated BOOT_KEY command/input merge, bounded SWD IDCODE command flow, debug
TX completion state, host tools, and
wired HIL helper's protocol checks. They do not prove USB enumeration, real NVS
power-loss persistence or purge behavior, BLE enumeration or radio lifetime,
real Wi-Fi provisioning, physical OTA persistence, bootloader rollback on a
board, a live ESP32-S3 TLS handshake/resource profile, authenticated TCP/DAP
dispatch over a real network link, target UART electrical timing or sustained
CDC throughput, or electrical SWD timing.
Follow `test/hil/wired.md` on a populated AirDAP board before marking roadmap
Stage 1 complete.
