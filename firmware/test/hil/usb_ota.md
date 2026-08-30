# USB OTA hardware-in-the-loop acceptance

Run this checklist on a recoverable AirDAP with the confirmed 16 MiB module.
Record the board revision, module ordering code, host OS, ESP-IDF version, USB
serial, build revisions, command output, and observed running versions. Keep a
serial programmer available: the first layout migration and recovery from a
deliberately broken test image require it.

## 1. Install the A/B baseline

Build a known-good revision A and retain its three binaries. Verify that the
decoded partition table contains `otadata`, two 4 MiB slots at `0x20000` and
`0x420000`, and no `factory` partition. Then perform the one-time full flash:

```sh
cd firmware
idf.py build
idf.py -p <airdap-programming-port> flash
python tools/airdap-update.py --help
```

After USB enumeration, record the `ADP-...` serial. Confirm normal CMSIS-DAP
and target-UART behavior before continuing.

## 2. Successful A to B update

Build a distinct known-good revision B, close every debugger using AirDAP, and
run:

```sh
python tools/airdap-update.py \
    --serial ADP-001122334455 \
    build/airdap.bin
```

Required observations:

- no reset or GPIO0 press is needed;
- upload reaches 100%, commit succeeds, and USB disconnects;
- the same USB serial reconnects within the configured timeout;
- the script reports revision B's running version;
- CMSIS-DAP and target UART still work after restart.

Power-cycle AirDAP, then install the same known-good B image once more. Record
that the updater reports B as both the previous and post-reconnect running
version; this also exercises the opposite OTA slot.

## 3. Interrupted upload retains the running slot

Start an update from B to another valid image and disconnect USB or press
Ctrl-C after progress begins but before commit. Reconnect power if necessary,
then verify that B still starts and that its CMSIS-DAP and target-UART functions
remain usable. Run the same update again from offset zero; resume from a prior
offset is intentionally unsupported.

## 4. Unconfirmed image rolls back

Create a disposable HIL-only revision C that deterministically calls
`esp_restart()` after USB initialization but before
`airdap_ota_confirm_running_image()`. Do not commit or distribute this fault
injection. Build C, install it through `airdap-update.py`, and retain the full
serial boot log.

Required observations:

- C is selected and starts once;
- C restarts before confirming `PENDING_VERIFY`;
- the ESP-IDF bootloader excludes C and boots the prior valid B slot;
- the same USB serial reconnects and B's running version is reported;
- another normal B-to-D update succeeds afterward.

If any step fails, recover with the serial programmer and the retained
baseline binaries. Host unit tests and a successful build do not replace these
power-loss and boot-selection observations.
