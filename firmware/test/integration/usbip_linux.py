#!/usr/bin/env python3
"""Exercise Linux vhci_hcd, usbfs Bulk and CDC with a LOCAL simulated backend.

Requires root, loaded vhci_hcd and cdc_acm modules. Never connects to AirDAP,
uses credentials, accesses physical USB devices, or runs target programming.
Only the selected free VHCI port is attached/detached; other ports are untouched.
"""

import asyncio
import ctypes
import fcntl
import logging
import os
from pathlib import Path
import select
import socket
import struct
import sys
import termios
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from airdap_usbip import Server


class LoopbackBackend:
    def __init__(self):
        self.rx = bytearray()
        self.coding = None

    async def connect(self):
        pass  # This backend has no external connection.

    async def close(self):
        pass

    async def dap(self, request):
        if request != b"\0\xff":
            raise AssertionError(f"unexpected test DAP request: {request.hex()}")
        return bytes.fromhex("0002fc01")

    async def configure(self, coding):
        self.coding = coding

    async def write(self, data):
        self.rx.extend(data)

    async def read(self, capacity):
        data = bytes(self.rx[:capacity])
        del self.rx[:capacity]
        return data

    async def keepalive(self):
        pass


def read_exact(connection, size):
    result = bytearray()
    while len(result) < size:
        chunk = connection.recv(size - len(result))
        if not chunk:
            raise AssertionError("short USB/IP import response")
        result.extend(chunk)
    return bytes(result)


def bulk(fd, ep, data_or_size):
    is_input = isinstance(data_or_size, int)
    size = data_or_size if is_input else len(data_or_size)
    buffer = ctypes.create_string_buffer(size) if is_input else ctypes.create_string_buffer(data_or_size)
    class Transfer(ctypes.Structure):
        _fields_ = [("ep", ctypes.c_uint), ("length", ctypes.c_uint),
                    ("timeout", ctypes.c_uint), ("data", ctypes.c_void_p)]
    transfer = Transfer(ep, size, 3000, ctypes.addressof(buffer))
    command = 0xC0005502 | (ctypes.sizeof(Transfer) << 16)
    result = fcntl.ioctl(fd, command, transfer)
    return bytes(buffer[:result]) if is_input else result


def exercise(port):
    vhci = Path("/sys/devices/platform/vhci_hcd.0")
    rows = [line.split() for line in (vhci / "status").read_text().splitlines()[1:]]
    free = next((row for row in rows if row[0] == "hs" and row[2] == "004"), None)
    if free is None:
        raise RuntimeError("no free VHCI USB2 port; no existing port was changed")
    number = int(free[1])
    attached = False
    with socket.create_connection(("127.0.0.1", port), 5) as connection:
        connection.sendall(bytes.fromhex("0111800300000000") + b"1-1".ljust(32, b"\0"))
        assert read_exact(connection, 8) == bytes.fromhex("0111000300000000")
        read_exact(connection, 312)
        try:
            (vhci / "attach").write_text(f"{number} {connection.fileno()} 65537 2")
            attached = True
            deadline = time.monotonic() + 10
            device = None
            while time.monotonic() < deadline:
                rows = [line.split() for line in (vhci / "status").read_text().splitlines()[1:]]
                row = next(row for row in rows if int(row[1]) == number)
                candidate = Path("/sys/bus/usb/devices") / row[-1]
                if (candidate / "serial").exists():
                    assert (candidate / "serial").read_text().strip() == "ADP-000000000000-NET"
                    device = candidate
                    break
                time.sleep(0.05)
            assert device is not None, "virtual device did not enumerate"
            busnum = int((device / "busnum").read_text())
            devnum = int((device / "devnum").read_text())
            node = f"/dev/bus/usb/{busnum:03d}/{devnum:03d}"
            with open(node, "r+b", buffering=0) as usb:
                fcntl.ioctl(usb.fileno(), 0x8004550F, struct.pack("I", 0))
                assert bulk(usb.fileno(), 1, b"\0\xff") == 2
                assert bulk(usb.fileno(), 0x81, 508) == bytes.fromhex("0002fc01")
            print("PASS: Linux VHCI enumeration and usbfs CMSIS-DAP DAP_Info")
            serial = None
            while time.monotonic() < deadline:
                matches = list(device.glob(f"{device.name}:1.1/tty/ttyACM*"))
                if matches:
                    serial = Path("/dev") / matches[0].name
                    if serial.exists():
                        break
                time.sleep(0.05)
            assert serial is not None and serial.exists(), "CDC ACM did not bind"
            fd = os.open(serial, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
            try:
                attrs = termios.tcgetattr(fd)
                attrs[0] = attrs[1] = attrs[3] = 0
                attrs[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
                attrs[4] = attrs[5] = termios.B115200
                attrs[6][termios.VMIN] = 0
                attrs[6][termios.VTIME] = 0
                termios.tcsetattr(fd, termios.TCSANOW, attrs)
                payload = b"AirDAP virtual CDC loopback\0\xff\r\n"
                assert os.write(fd, payload) == len(payload)
                received = b""
                deadline = time.monotonic() + 5
                while len(received) < len(payload) and time.monotonic() < deadline:
                    if select.select([fd], [], [], 0.1)[0]:
                        received += os.read(fd, 4096)
                assert received == payload, f"CDC data mismatch: {received!r}"
                print("PASS: Linux cdc_acm tty enumeration, line coding and binary loopback")
            finally:
                os.close(fd)
        finally:
            if attached:
                (vhci / "detach").write_text(str(number))


async def main():
    if os.geteuid() != 0:
        raise SystemExit("run as root after loading vhci_hcd and cdc_acm")
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
    asyncio.run(main())
