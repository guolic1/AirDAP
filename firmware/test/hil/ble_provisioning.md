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

Restart and monitor AirDAP. Record `button bindings`, then use `button defaults`.
The following steps assume the default mappings; restore custom bindings after
acceptance. Confirm BLE is not advertising before a button
press. Hold `BOOT_KEY` until the red STATUS LED starts its 500 ms on/off
slow flash at two seconds. NET must remain off.
Before releasing it, confirm BLE has not initialized or started advertising
and an attached debug-shell session remains connected. Release the button and
confirm:

- the `ADP-...` BLE service appears;
- its name matches the USB/device identity;
- the logged credential fingerprint is
  `C5D2AA01B4DDA9A67CBE111D61B9F0CBBD3A9F7A7935E85E570A881C7EE03080`;
- no SSID or Wi-Fi password appears in logs.

Use the AirDAP provisioning tool. It discovers active AirDAP advertisements and
supplies the public Security 2 credential automatically; the Wi-Fi passphrase
is still prompted privately:

```sh
python tools/airdap-provision.py
```

After the client connects, confirm the device reports a negotiated ATT MTU of
at least 409 bytes, so the 406-byte Security 2 session request fits in one
characteristic write, and the request is not rejected with `Invalid PDU`.

On Windows, also verify with a device that is not paired in OS settings:
connection must proceed without a system pairing request or the approximately
60-second device-association wait. Confirm Security 2 establishes a session and
the Wi-Fi selection prompt appears. For this connection-only regression check,
cancel at that prompt without submitting credentials, then close the BLE
window and verify the previous Wi-Fi connection resumes. Repeat on Linux when
a Linux Bluetooth host is available; host unit tests do not establish RF timing.

## 2. Authentication failure and cancellation

Open a new window and use the AirDAP launcher for this negative-only test,
substituting the observed device ID and a deliberately incorrect PoP:

```sh
python tools/airdap_esp_prov.py \
    managed_components/espressif__network_provisioning/tool/esp_prov/esp_prov.py \
    --transport ble \
    --service_name <ADP-device-id> \
    --sec_ver 2 \
    --sec2_username wifiprov \
    --sec2_pwd intentionally-wrong-pop
```

Confirm the secure session is rejected, the stored Wi-Fi configuration is
unchanged, and the BLE window remains available. Hold `BOOT_KEY` until the
red STATUS LED starts flashing slowly again, then release it. Confirm BLE advertising and
the provisioning service stop, the mode no longer reports an active
provisioning attempt, and the prior Wi-Fi configuration remains in effect.

## 3. Timeout cleanup

Open another window and do not connect a client. Confirm the service disappears
after 120 seconds and does not return without a new two-second
hold-and-release. Confirm the previously committed Wi-Fi configuration and
normal USB interfaces remain usable. This is the resource-lifecycle acceptance
check; a client disconnect alone is not evidence that the firmware stopped the
BLE service.

## 4. First provisioning and reboot recovery

If necessary, perform the ten-second reset from section 5 first. Open a window,
run the AirDAP provisioning tool, select the dedicated test AP, and enter its
password interactively. Required observations:

- the Security 2 session succeeds and Wi-Fi association reaches DHCP;
- the device becomes provisioned and online;
- the client reports provisioning success before the service disappears;
- BLE stops and releases its resources within 30 seconds after success;
- a power cycle reconnects to the same AP without opening BLE;
- a fresh two-second hold-and-release can open a new window using the same public
  Security 2 credential fingerprint.

Repeat with an incorrect AP password before the successful attempt. Confirm the
failed candidate is not published. For the reset-specific check, reconnect the
raw Espressif client command from section 2 with the correct public PoP and
`--reset`, then rerun `python tools/airdap-provision.py` and enter the correct AP
credentials. Confirm the prior committed Wi-Fi configuration is restored on
cancel/timeout and no credential value appears in logs except the documented
public fingerprint.

## 5. Ten-second network reset and GPIO0 release guard

With the device provisioned, hold `BOOT_KEY` continuously. Confirm STATUS stays
off before two seconds, then flashes with 500 ms on/off without initializing
BLE or disconnecting the debug shell. Continue holding until STATUS changes to
200 ms on/off at six seconds, then 60 ms on/off at ten seconds. NET must remain
off throughout. Keep it pressed briefly and confirm AirDAP
has not cleared configuration or restarted while GPIO0 remains low. Release the
button and confirm both LEDs turn off before AirDAP clears configuration and
restarts normally rather than entering the ROM download mode. The debug shell
disconnect caused by that restart is expected. After restart:

- provisioning state is `unprovisioned` and Wi-Fi does not reconnect;
- the reserved pairing and network-authentication slots are absent;
- BLE remains off until another two-second hold-and-release;
- the new window reports the same Security 2 credential fingerprint and
  accepts the public PoP.

Finish by clearing the test Wi-Fi configuration. Record that anyone within BLE
range who knows the public credential can provision during a physically opened
window; this test does not establish per-device owner authentication.

## 6. Click feedback

With no provisioning window open, press and release BOOT_KEY briefly. STATUS
must stay off while pressed and while waiting for a second press; 300 ms after
release begins, confirm one 100 ms flash and one single-click log. Repeat with
two short presses separated by less than 300 ms: confirm two 100 ms flashes
separated by 100 ms off, one double-click log, and no single-click log. Neither
gesture should start BLE, reset either MCU, or change target power. NET stays
off. Start another ordinary press during a completion flash and confirm its
40 ms press confirmation cancels the remaining flash sequence.

Observe long-hold release bounce if suitable test equipment is available: a
release shorter than 200 ms must retain the current flash phase and not execute
the action or repeat threshold events. Releasing between 6 and 10 seconds with
default bindings must log `hold6 command=none` and never open BLE or clear data.
Record measured timing separately from
host-test results; polling and task scheduling can affect real flash durations.

## 7. Persistent bindings and DAP selection

Record `button commands` (seven entries) and `button bindings` (five gestures).
Set `button bind hold6 dap-auto`, restart and confirm the binding survives while
`dap-route=auto`. Restore the defaults and confirm that this change survives a
second restart. Invalid gestures/commands or trailing tokens must fail without
changing the stored bindings. Save and restore any pre-test custom mappings.

With USB data attached, Wi-Fi online, and DAP disconnected, double-click and
confirm `dap-route=network`. Verify authenticated pyOCD DAP access over TCP and
verify that USB DAP cannot acquire the target. USB CDC and debug shell should
remain enumerated. Unauthenticated network access must remain rejected.
Disconnect DAP and double-click again; confirm `dap-route=usb`, wired DAP works,
and network DAP is rejected. With a DAP owner or OTA active, verify the selection
command reports busy and preserves the current route and transfer. Restart and
confirm automatic selection is restored. Explicit NETWORK selection must survive
USB detach/reattach until another command or reboot changes the selection.

On a recoverable board only, map double-click to `clear-network-restart` and
observe that restart cannot occur before 200 ms of stable release. A new press
during that guard must cancel the pending clear. Restore defaults afterward.
