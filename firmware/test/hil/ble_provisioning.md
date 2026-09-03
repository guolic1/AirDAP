# BLE Security 2 provisioning hardware-in-the-loop acceptance

Run this checklist only on an explicitly selected, recoverable development
AirDAP and a dedicated test access point. Record the board revision, module
ordering code, ESP-IDF version, firmware revision, AirDAP device ID, test AP,
client OS/Bluetooth adapter, and the Security 2 credential fingerprint. Never
record the Wi-Fi password. The Security 2 username and PoP are intentionally
public.

This procedure writes flash and changes Wi-Fi configuration. Obtain approval
for the selected board before running it. Host tests and a successful ESP-IDF
build do not replace the RF, persistence, GPIO0, and restart observations.

## 1. Build the public Security 2 credential

Activate ESP-IDF and build the standard firmware. The public username
`wifiprov`, PoP `abcd1234`, salt, and verifier are already compiled into the
application; no credential image or injection step is required:

```sh
cd firmware
idf.py build
idf.py -p <airdap-programming-port> flash
```

Restart and monitor AirDAP. Confirm BLE is not advertising before a button
press. Hold `BOOT_KEY` for three seconds, release it, and confirm:

- the `ADP-...` BLE service appears;
- its name matches the USB/device identity;
- the logged credential fingerprint is
  `C5D2AA01B4DDA9A67CBE111D61B9F0CBBD3A9F7A7935E85E570A881C7EE03080`;
- no SSID or Wi-Fi password appears in logs.

Use the Espressif provisioning client with the public Security 2 credential;
leave the Wi-Fi passphrase unset so it is prompted privately:

```sh
python managed_components/espressif__network_provisioning/tool/esp_prov/esp_prov.py \
    --transport ble \
    --service_name <ADP-device-id> \
    --sec_ver 2 \
    --sec2_username wifiprov \
    --sec2_pwd abcd1234
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
enter the public PoP, select the dedicated test AP, and enter its password
interactively. Required observations:

- the Security 2 session succeeds and Wi-Fi association reaches DHCP;
- the device becomes provisioned and online;
- the client reports provisioning success before the service disappears;
- BLE stops and releases its resources within 30 seconds after success;
- a power cycle reconnects to the same AP without opening BLE;
- a fresh three-second press can open a new window using the same public
  Security 2 credential fingerprint.

Repeat with an incorrect AP password before the successful attempt. Confirm the
failed candidate is not published. Reconnect the same client with `--reset` in
addition to the Security 2 arguments above, then rerun without `--reset` and
enter the correct AP credentials. Confirm the prior committed Wi-Fi
configuration is restored on cancel/timeout and no credential value appears in
logs except the documented public fingerprint.

## 5. Ten-second network reset and GPIO0 release guard

With the device provisioned, hold `BOOT_KEY` continuously for ten seconds.
Keep it pressed briefly after the clear action and confirm AirDAP does not
restart while GPIO0 remains low. Release the button and confirm it restarts
normally rather than entering the ROM download mode. After restart:

- provisioning state is `unprovisioned` and Wi-Fi does not reconnect;
- the reserved pairing and network-authentication slots are absent;
- BLE remains off until another three-second press;
- the new window reports the same Security 2 credential fingerprint and
  accepts the public PoP.

Finish by clearing the test Wi-Fi configuration. Record that anyone within BLE
range who knows the public credential can provision during a physically opened
window; this test does not establish per-device owner authentication.
