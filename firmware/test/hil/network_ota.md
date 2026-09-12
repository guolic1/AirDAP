# Authenticated network OTA acceptance

Reserve one identified, recoverable 8 MiB AirDAP for the entire run. Wait for
all other HIL agents to finish and acknowledge release before opening USB,
BLE, CDC or network clients. Keep sole ownership through rollback/recovery;
close all handles and explicitly release the device afterward. Run hardware
tests serially. Retain a valid baseline image and a recovery path. Record
device ID, IP, port, credential path (never contents), commits, image hashes,
before/after QUERY, logs and each assertion.

## Baseline

Run host tests and ESP-IDF build gates first. Verify the merged TCP 3260
control and TCP 3261 UART baseline, including shared token and disconnect
revocation. If the running firmware lacks network OTA, install the new image
with the existing USB OTA tool. Confirm the same identity and VALID image,
then close the shell and query through `airdap-network-update.py --query`.

An existing baseline can report UART TX busy after re-enumeration because
host line-coding activity acquired USB TX. Record this separately; opening
and normally closing the selected CDC port releases that session. This
workaround is not evidence that OTA permanently owns UART.

## Rejections and cleanup

Use the Python Client in `firmware/tools/airdap-network-update.py` for bounded
fault injection. Use the next frame sequence except in deliberate replay tests.

1. Wrong PSK must fail TLS. HELLO without AUTH must reject every OTA opcode.
   TCP 3261 must reject OTA opcodes even after authentication.
2. Save QUERY's addresses/version. BEGIN zero or beyond capacity must fail.
3. Acquire USB DAP (or NETWORK DAP with USB absent) and network UART TX. Join
   the owner's token for control as needed. BEGIN must release both owners;
   DAP operations and both UART TX acquisitions must remain blocked while
   receiving/committed. Closing a bound sibling intentionally revokes all
   owner connections, so keep them open until testing that failure path.
4. Reject wrong offset, overrun and incomplete COMMIT. ABORT restores admission
   without selecting the incomplete slot. A duplicate COMMIT is invalid state.
5. Close TLS mid-upload and mid-frame; also test owner idle expiry and a bound
   sibling disconnect. After teardown, a new owner can BEGIN at offset zero.
   Query retains the original running version/address. Complete a later update
   to prove recovery rather than relying solely on the status response.
6. Upload a corrupted image. COMMIT must fail validation and leave the valid
   image bootable. Do not corrupt boot metadata.

## Successful updates

Build known-good revisions A and B with distinct Git-derived versions. Run
`airdap-network-update.py <ip> <B.bin> --credential <path>` from A. Require
identity match, running address equal to A's inactive address, version B and
confirmed=true. Repeat B to A. Also close the connection after COMMIT, reconnect
and REBOOT; boot selection must survive disconnect, and the ACK must precede
restart. Recheck USB DAP, UART and authenticated control afterward.

## Unconfirmed rollback

Use a disposable untracked copy of B that calls `esp_restart()` after USB
initialization but before `airdap_ota_confirm_running_image()`. Build with the
normal rollback/artifact gates. Never disable gates or burn eFuses. Upload
through authenticated network OTA from valid A. Expect the normal updater to
fail its final verification because B is not the final running image. Retain
serial logs proving B starts PENDING_VERIFY, restarts unconfirmed, and the
bootloader returns to valid A. Confirm identity, original address/version and
confirmed=true, then complete another normal update.

If serial capture or recovery conditions are unavailable, record rollback as
unverified. Unit tests do not prove physical boot selection or persistence.
