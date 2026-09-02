# mDNS discovery HIL

This procedure proves the RF and LAN boundary that host unit tests cannot:
AirDAP must publish its current station address as `_airdap._tcp.local` after
DHCP succeeds, replace the record after an address change, and stop publishing
when the station loses its IP. It also compares the TXT firmware value with the
same `device_identity` value exposed over USB.

Flashing, updating, restarting, or changing Wi-Fi credentials requires explicit
authorization for the selected development board. Use a dedicated test AP and
non-production credentials. Do not place credentials in command arguments,
logs, captures, or this repository.

## Preconditions

1. Build and install the Debug Shell profile described in
   [`../../README.md`](../../README.md), preserving the standard AirDAP
   partition table. Record the device USB serial and built commit.
2. From Windows, run `uv run python firmware/tools/airdap-shell.py`, then use
   interactive `wifi set`. Wait until `wifi status` reports `wifi=online`.
3. Keep the shell open so `mDNS service published` and `mDNS service withdrawn`
   lifecycle logs can be correlated with host observations. Do not treat logs
   alone as proof of a LAN announcement.
4. Put one Linux host and one Windows host on the same broadcast domain as the
   test AP. Linux needs Avahi client tools. Windows needs a DNS-SD browser such
   as Apple's `dns-sd.exe`.

## Initial announcement and identity

On Linux:

```sh
avahi-browse --resolve --terminate _airdap._tcp
```

On Windows, first browse for the instance and then resolve the exact instance
name returned by the browse:

```powershell
dns-sd.exe -B _airdap._tcp local
dns-sd.exe -L ADP-001122334455 _airdap._tcp local
```

For both hosts, record and check:

- one instance named exactly like the USB `device_id`;
- hostname `airdap-<12 lowercase MAC digits>.local`;
- the current DHCP address and SRV port 3260;
- exactly the seven TXT keys `id`, `proto`, `fw`, `cap`, `state`, `dap_port`,
  and `uart_port`;
- `id` equals the USB device ID, `fw` equals the Debug Shell `identity`
  firmware version, `proto=1`, `state=idle`, `dap_port=3260`, and
  `uart_port=3261`;
- no SSID, password, PoP, PSK, token, or other credential appears.

## Address replacement and offline withdrawal

1. Use the test AP's DHCP controls to give the board a different address, then
   reconnect it without rebuilding the firmware.
2. Wait for `wifi=online` and a new `mDNS service published` log. Repeat the
   Linux and Windows resolve commands and confirm that only the new address is
   returned by a fresh query.
3. Disconnect or power down the test AP. Confirm `wifi status` no longer reports
   online and observe `mDNS service withdrawn` after the station loses its IP.
4. A host cache may retain the old record until its TTL expires. Do not count a
   cached entry as a live advertisement; use a fresh browse after cache expiry
   or capture new multicast responses and confirm the offline device sends
   none.
5. Restore the AP. Confirm automatic Wi-Fi recovery and resolve the service
   again from both hosts. The record must contain the current address and the
   same device ID and firmware version, with no stale duplicate instance.

Record the board serial, firmware version, AP/test-host OS versions, old and new
addresses, command outputs, and relevant timestamps outside Git. This procedure
does not authenticate the discovered device or prove that the future TCP
services are listening.
