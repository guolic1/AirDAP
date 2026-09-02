# BLE Security 2 provisioning hardware-in-the-loop acceptance

Run this checklist only on an explicitly selected, recoverable development
AirDAP and a dedicated test access point. Record the board revision, module
ordering code, ESP-IDF version, firmware revision, AirDAP device ID, test AP,
client OS/Bluetooth adapter, and the Security 2 credential fingerprint. Never
record the PoP, Wi-Fi password, salt, verifier, or private NVS image.

This procedure writes flash and changes Wi-Fi configuration. Obtain approval
for the selected board before running it. Host tests and a successful ESP-IDF
build do not replace the RF, persistence, GPIO0, and restart observations.

## 1. Install development Security 2 credentials

Activate ESP-IDF and build the standard firmware. Generate the credential
image interactively; use a unique test-only PoP that is not used by any
production system:

```sh
cd firmware
idf.py build
python tools/airdap-sec2-credentials.py /absolute/private/path/sec2_keys.bin
```

Retain the printed fingerprint in the evidence record. Perform the baseline
flash, then inject only the new read-only partition:

```sh
idf.py -p <airdap-programming-port> flash
python "$IDF_PATH/components/partition_table/parttool.py" \
    --port <airdap-programming-port> \
    write_partition \
    --partition-name sec2_keys \
    --input /absolute/private/path/sec2_keys.bin \
    --ignore-readonly
```

Restart and monitor AirDAP. Confirm BLE is not advertising before a button
press. Hold `BOOT_KEY` for three seconds, release it, and confirm:

- the `ADP-...` BLE service appears;
- its name matches the USB/device identity;
- the logged credential fingerprint exactly matches the generator output;
- no salt, verifier, PoP, SSID, or password appears in logs.

Use the Espressif provisioning client without command-line passwords:

```sh
python managed_components/espressif__network_provisioning/tool/esp_prov/esp_prov.py \
    --transport ble \
    --service_name <ADP-device-id> \
    --sec_ver 2 \
    --sec2_username airdap
```

## 2. Authentication failure and cancellation

Open a new window and enter a deliberately incorrect PoP. Confirm the secure
session is rejected, the stored Wi-Fi configuration is unchanged, and the BLE
window remains available. Hold `BOOT_KEY` for three seconds again. Confirm BLE
advertising and the provisioning service stop, the mode no longer reports an
active provisioning attempt, and the prior Wi-Fi configuration remains in
effect.

## 3. Timeout cleanup

Open another window and do not connect a client. Confirm the service disappears
after 120 seconds and does not return without a new three-second press. Confirm
the previously committed Wi-Fi configuration and normal USB interfaces remain
usable. This is the resource-lifecycle acceptance check; a client disconnect
alone is not evidence that the firmware stopped the BLE service.

## 4. First provisioning and reboot recovery

If necessary, perform the ten-second reset from section 5 first. Open a window,
enter the correct test PoP, select the dedicated test AP, and enter its password
interactively. Required observations:

- the Security 2 session succeeds and Wi-Fi association reaches DHCP;
- the device becomes provisioned and online;
- the client reports provisioning success before the service disappears;
- BLE stops and releases its resources within 30 seconds after success;
- a power cycle reconnects to the same AP without opening BLE;
- a fresh three-second press can open a new window using the same injected
  Security 2 credential fingerprint.

Repeat with an incorrect AP password before the successful attempt. Confirm the
failed candidate is not published. Reconnect the same client with `--reset` in
addition to the Security 2 arguments above, then rerun without `--reset` and
enter the correct AP credentials. Confirm the prior committed Wi-Fi
configuration is restored on cancel/timeout and no credential value appears in
logs.

## 5. Ten-second network reset and GPIO0 release guard

With the device provisioned, hold `BOOT_KEY` continuously for ten seconds.
Keep it pressed briefly after the clear action and confirm AirDAP does not
restart while GPIO0 remains low. Release the button and confirm it restarts
normally rather than entering the ROM download mode. After restart:

- provisioning state is `unprovisioned` and Wi-Fi does not reconnect;
- the reserved pairing and network-authentication slots are absent;
- BLE remains off until another three-second press;
- the new window reports the same Security 2 credential fingerprint and
  accepts the original test PoP.

Finish by clearing the test Wi-Fi configuration. Securely delete the local PoP
record and the repository-external `sec2_keys.bin` when the board no longer
needs this HIL credential. If an erase-flash operation is used, the Security 2
image must be injected again before BLE provisioning can start.
