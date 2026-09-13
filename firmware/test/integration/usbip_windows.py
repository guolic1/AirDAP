#!/usr/bin/env python3
"""Verify installed Windows VHCI, WinUSB and COM using a local fake AirDAP.

Uses the repository's PyUSB/libusb-package dependencies. No AirDAP credential,
network connection to a board, or target flash operation is involved. Only the
new virtual port is detached; existing USB/IP ports and services are untouched.
"""

import asyncio
import ctypes
from ctypes import wintypes
import json
import logging
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

import libusb_package
import usb.core
import usb.util

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from airdap_usbip import Server


class LoopbackBackend:
    def __init__(self):
        self.rx = bytearray()

    async def connect(self):
        pass

    async def close(self):
        pass

    async def dap(self, request):
        assert request == b"\0\xff"
        return bytes.fromhex("0002fc01")

    async def configure(self, coding):
        assert coding[0] > 0

    async def write(self, data):
        self.rx.extend(data)

    async def read(self, capacity):
        data = bytes(self.rx[:capacity])
        del self.rx[:capacity]
        return data

    async def keepalive(self):
        pass


def serial_loopback(name):
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
        ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.GetCommState.argtypes = kernel.SetCommState.argtypes = [wintypes.HANDLE, ctypes.c_void_p]
    kernel.BuildCommDCBW.argtypes = [wintypes.LPCWSTR, ctypes.c_void_p]
    kernel.SetCommTimeouts.argtypes = [wintypes.HANDLE, ctypes.c_void_p]
    for function in (kernel.WriteFile, kernel.ReadFile):
        function.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD,
                             ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
    handle = kernel.CreateFileW("\\\\.\\" + name, 0xC0000000, 0, None, 3, 0, None)
    if handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    def check(result):
        if not result:
            raise ctypes.WinError(ctypes.get_last_error())
    try:
        # DCB is a 28-byte Windows ABI structure; BuildCommDCB sets named fields.
        dcb = ctypes.create_string_buffer(28)
        ctypes.c_uint32.from_buffer(dcb).value = 28
        check(kernel.GetCommState(handle, dcb))
        check(kernel.BuildCommDCBW("baud=115200 parity=n data=8 stop=1 dtr=on rts=off", dcb))
        check(kernel.SetCommState(handle, dcb))
        timeouts = (wintypes.DWORD * 5)(50, 0, 1000, 0, 1000)
        check(kernel.SetCommTimeouts(handle, timeouts))
        payload = b"AirDAP virtual Windows COM\0\xff\r\n"
        count = wintypes.DWORD()
        check(kernel.WriteFile(handle, payload, len(payload), ctypes.byref(count), None))
        assert count.value == len(payload)
        received = b""
        deadline = time.monotonic() + 5
        while len(received) < len(payload) and time.monotonic() < deadline:
            buffer = ctypes.create_string_buffer(4096)
            check(kernel.ReadFile(handle, buffer, len(buffer), ctypes.byref(count), None))
            received += buffer.raw[:count.value]
        assert received == payload, f"COM data mismatch: {received!r}"
    finally:
        kernel.CloseHandle(handle)


def exercise(port):
    executable = shutil.which("usbip.exe")
    if executable is None:
        raise RuntimeError("installed usbip-win2 client is required")
    result = subprocess.run([executable, "-t", str(port), "attach", "-r", "127.0.0.1",
                             "-b", "1-1", "--once", "--terse"],
                            capture_output=True, text=True, encoding="utf-8", timeout=20, check=True)
    number = result.stdout.strip()
    if not re.fullmatch(r"\d+", number):
        raise RuntimeError(f"unexpected attach port output: {result.stdout!r} {result.stderr!r}")
    try:
        deadline = time.monotonic() + 20
        device = None
        while time.monotonic() < deadline:
            device = usb.core.find(idVendor=0x303A, idProduct=0x4021,
                backend=libusb_package.get_libusb1_backend(),
                custom_match=lambda d: d.serial_number == "ADP-000000000000-NET")
            if device is not None:
                break
            time.sleep(0.1)
        assert device is not None, "virtual WinUSB device did not enumerate"
        try:
            usb.util.claim_interface(device, 0)
            assert device.write(1, b"\0\xff", timeout=3000) == 2
            assert bytes(device.read(0x81, 508, timeout=3000)) == bytes.fromhex("0002fc01")
        finally:
            usb.util.dispose_resources(device)
        print("PASS: Windows VHCI enumeration, WinUSB binding and CMSIS-DAP DAP_Info")
        # Identify only the serial function whose parent is our fake device.
        command = r"""
[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new()
$ErrorActionPreference = 'Stop'
Get-PnpDevice -PresentOnly -Class Ports | ForEach-Object {
    $parent = Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_Parent
    if ($parent.Data -eq 'USB\VID_303A&PID_4021\ADP-000000000000-NET') {
        $_.FriendlyName
    }
} | ConvertTo-Json -Compress
"""
        result = subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", command],
                                capture_output=True, text=True, encoding="utf-8", timeout=30, check=True)
        friendly = json.loads(result.stdout)
        assert isinstance(friendly, str), f"expected one test COM port: {friendly!r}"
        match = re.search(r"\((COM\d+)\)", friendly)
        assert match, f"missing COM port in {friendly!r}"
        serial_loopback(match[1])
        print("PASS: Windows usbser COM enumeration, line coding and binary loopback")
    finally:
        subprocess.run([executable, "detach", "-p", number], check=True, timeout=15)


async def main():
    if os.name != "nt":
        raise SystemExit("run this integration check on Windows")
    bridge = Server("ADP-000000000000", LoopbackBackend)
    server = await asyncio.start_server(bridge.handle, "127.0.0.1", 0)
    try:
        await asyncio.to_thread(exercise, server.sockets[0].getsockname()[1])
    finally:
        server.close()
        await server.wait_closed()
        await bridge.close()


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO)
    asyncio.run(main(), loop_factory=asyncio.SelectorEventLoop)
