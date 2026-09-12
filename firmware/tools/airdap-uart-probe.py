#!/usr/bin/env python3
"""Chunked authenticated UART loopback; GPIO17/18 must be connected."""

from __future__ import annotations

import argparse
import importlib.util
import json
import random
import secrets
import socket
import ssl
import struct
import sys
import time
from pathlib import Path

from airdap_network_credential import CredentialError, load_credential

# Reuse the established v1 frame/HELLO/AUTH validation from the DAP probe.
_spec = importlib.util.spec_from_file_location(
    "airdap_dap_probe", Path(__file__).with_name("airdap-dap-probe.py")
)
dap = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = dap
_spec.loader.exec_module(dap)
ProbeError = dap.ProbeError

STATUS, ACQUIRE, CONFIGURE, WRITE, READ = range(0x10, 0x15)


class Client:
    def __init__(self, host, credential, *, port=3261, token=b"", timeout=5.0,
                 bind=True):
        self.frame_session = secrets.randbelow(dap.UINT32_MAX) + 1
        self.sequence = 0
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.minimum_version = context.maximum_version = ssl.TLSVersion.TLSv1_3
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE

        def psk(hint):
            if hint not in (None, credential.identity):
                raise ProbeError("PSK identity hint mismatch")
            return credential.identity, credential.psk

        context.set_psk_client_callback(psk)
        raw = socket.create_connection((host, port), timeout=timeout)
        try:
            self.connection = context.wrap_socket(raw, server_hostname=None)
        except BaseException:
            raw.close()
            raise
        try:
            if (self.connection.version() != dap.EXPECTED_TLS_VERSION or
                    self.connection.cipher()[0] != dap.EXPECTED_CIPHER):
                raise ProbeError("unexpected TLS version or cipher")
            hello = self.request(1, b"", 1)
            _, _, self.firmware = dap._parse_hello(hello, credential.device_id)
            self.owner_session = 0
            self.token = b""
            if bind:
                self.owner_session, self.token = dap._parse_auth(self.request(2, token, 2))
        except BaseException:
            self.close()
            raise

    def close(self):
        self.connection.close()
        self.token = b""

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def request(self, kind, payload, expected):
        self.sequence = self.sequence % dap.UINT32_MAX + 1
        self.connection.sendall(dap._encode_frame(
            kind, self.frame_session, self.sequence, payload))
        return dap._read_response(self.connection, expected_type=expected,
                                 session_id=self.frame_session, sequence=self.sequence)

    def control(self, opcode, data=b""):
        response = self.request(5, bytes([opcode]) + data, 6)
        if not response or response[0] != opcode:
            raise ProbeError("UART response opcode mismatch")
        return response[1:]

    def acquire(self):
        if self.control(ACQUIRE):
            raise ProbeError("ACQUIRE response must contain only its opcode")

    def configure(self, baud, stop=0, parity=0, bits=8):
        if self.control(CONFIGURE, struct.pack(">IBBB", baud, stop, parity, bits)):
            raise ProbeError("CONFIGURE response must contain only its opcode")

    def status(self):
        response = self.control(STATUS)
        if len(response) != 14:
            raise ProbeError("invalid UART status length")
        baud, stop, parity, bits, owner, buffered, dropped = struct.unpack(">IBBBBHI", response)
        if owner not in (0, 1) or buffered > 512:
            raise ProbeError("invalid UART status fields")
        return dict(baud=baud, stop=stop, parity=parity, bits=bits,
                    tx_owner=bool(owner), buffered=buffered, dropped=dropped)

    def write(self, payload):
        if not 1 <= len(payload) <= 256:
            raise ProbeError("WRITE size must be 1..256")
        response = self.control(WRITE, payload)
        if len(response) != 2:
            raise ProbeError("invalid WRITE response length")
        count = int.from_bytes(response, "big")
        if count > len(payload):
            raise ProbeError("WRITE accepted more bytes than sent")
        return count

    def write_all(self, payload, timeout=5):
        deadline = time.monotonic() + timeout
        offset = 0
        while offset < len(payload):
            if time.monotonic() >= deadline:
                raise ProbeError("UART TX acceptance timed out")
            count = self.write(payload[offset:offset + 256])
            offset += count
            if count == 0:
                time.sleep(0.01)

    def read(self, capacity=256):
        if not 1 <= capacity <= 256:
            raise ProbeError("READ capacity must be 1..256")
        response = self.control(READ, struct.pack(">H", capacity))
        if not 4 <= len(response) <= 4 + capacity:
            raise ProbeError("invalid READ response length")
        return response[4:], int.from_bytes(response[:4], "big")

    def read_exact(self, size, timeout=5):
        deadline = time.monotonic() + timeout
        result = bytearray()
        while len(result) < size:
            if time.monotonic() >= deadline:
                raise ProbeError(f"UART RX timed out at {len(result)}/{size} bytes")
            data, dropped = self.read(min(256, size - len(result)))
            if dropped:
                raise ProbeError(f"UART subscriber dropped {dropped} bytes")
            result.extend(data)
            if not data:
                time.sleep(0.01)
        return bytes(result)


def expect_error(operation, code):
    try:
        operation()
    except ProbeError as error:
        if str(error) == f"AirDAP error 0x{code:04X}":
            return
        raise
    raise ProbeError(f"operation did not reject with 0x{code:04X}")


def loopback(host, credential, total, chunk, reconnects):
    cases = []
    for baud in (9600, 115200, 1000000):
        with Client(host, credential) as client:
            client.acquire()
            client.configure(baud)
            configured = client.status()
            if configured["baud"] != baud or not configured["tx_owner"]:
                raise ProbeError("UART configuration/ownership mismatch")
            expect_error(lambda: client.configure(0), 0x23)
            if client.status() != configured:
                raise ProbeError("invalid configuration changed UART state")
            rng = random.Random(baud)
            started = time.monotonic()
            for offset in range(0, total, chunk):
                data = rng.randbytes(min(chunk, total - offset))
                client.write_all(data)
                if client.read_exact(len(data)) != data:
                    raise ProbeError(f"loopback mismatch at baud={baud} offset={offset}")
            cases.append(dict(baud=baud, bytes=total, chunk=chunk,
                              seconds=round(time.monotonic() - started, 3),
                              dropped=client.status()["dropped"], firmware=client.firmware))
        time.sleep(0.15)
    for cycle in range(reconnects):
        with Client(host, credential) as client:
            client.acquire()
            client.configure(115200)
            payload = bytes([cycle % 256]) * chunk
            client.write_all(payload)
            if client.read_exact(chunk) != payload:
                raise ProbeError(f"reconnect loopback mismatch at cycle {cycle}")
        time.sleep(0.15)
    return dict(cases=cases, reconnects=reconnects, reconnect_bytes=reconnects * chunk)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host")
    parser.add_argument("--credential", required=True, type=Path)
    parser.add_argument("--bytes", type=int, default=4096)
    parser.add_argument("--chunk", type=int, default=256)
    parser.add_argument("--reconnects", type=int, default=10)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    if args.bytes <= 0 or not 1 <= args.chunk <= 256 or args.reconnects < 0:
        raise ProbeError("invalid byte count, chunk size or reconnect count")
    evidence = loopback(args.host, load_credential(args.credential),
                        args.bytes, args.chunk, args.reconnects)
    text = json.dumps(evidence, indent=2) + "\n"
    if args.output:
        args.output.write_text(text, encoding="utf-8")
    print(text)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ProbeError, CredentialError, OSError) as error:
        print(f"airdap-uart-probe: {error}", file=sys.stderr)
        raise SystemExit(1)
