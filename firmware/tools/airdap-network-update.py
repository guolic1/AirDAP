#!/usr/bin/env python3
"""Update AirDAP through authenticated CONTROL on TCP 3260."""

from __future__ import annotations

import argparse
import importlib.util
import json
import struct
import sys
import time
from dataclasses import asdict, dataclass
from pathlib import Path

from airdap_network_credential import CredentialError, load_credential
from verify_ota_layout import VerificationError, validate_runtime_slots

_spec = importlib.util.spec_from_file_location(
    "airdap_uart_probe", Path(__file__).with_name("airdap-uart-probe.py"))
uart = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(uart)
UpdateError = uart.ProbeError
QUERY, BEGIN, WRITE, COMMIT, ABORT, REBOOT = range(0x30, 0x36)
MAX_CHUNK = 4091
STATUS_NAMES = {1: "invalid argument", 2: "invalid state", 3: "invalid size",
    4: "invalid offset", 5: "incomplete image", 6: "flash write failed",
    7: "image validation failed", 8: "activation failed", 9: "internal error"}


@dataclass(frozen=True)
class OtaInfo:
    capacity: int
    running_address: int
    update_address: int
    confirmed: bool
    version: str


class Client(uart.Client):
    def __init__(self, host, credential, *, port=3260, timeout=10.0):
        super().__init__(host, credential, port=port, timeout=timeout)

    def control(self, opcode, data=b""):
        response = self.request(5, bytes([opcode]) + data, 6)
        if len(response) < 2 or response[0] != opcode:
            raise UpdateError("OTA response opcode or length mismatch")
        if response[1]:
            if len(response) != 2:
                raise UpdateError("malformed OTA failure response")
            raise UpdateError("OTA " + STATUS_NAMES.get(response[1], f"unknown status {response[1]}"))
        return response[2:]

    def query(self):
        response = self.control(QUERY)
        if not 16 <= len(response) <= 47:
            raise UpdateError("invalid OTA QUERY response length")
        protocol, flags, capacity, running, inactive, confirmed = struct.unpack(">BBIIIB", response[:15])
        if protocol != 1 or flags != 1 or confirmed not in (0, 1) or capacity == 0 or running == inactive:
            raise UpdateError("invalid OTA QUERY fields or rollback capability")
        try:
            validate_runtime_slots(capacity, running, inactive)
        except VerificationError as error:
            raise UpdateError(str(error)) from error
        try:
            version = response[15:].decode("utf-8")
        except UnicodeDecodeError as error:
            raise UpdateError("invalid OTA version text") from error
        if not version or "\0" in version or version != self.firmware:
            raise UpdateError("OTA QUERY and authenticated HELLO versions disagree")
        return OtaInfo(capacity, running, inactive, bool(confirmed), version)

    def _empty(self, opcode, data=b""):
        if self.control(opcode, data):
            raise UpdateError("unexpected OTA response data")

    def begin(self, size): self._empty(BEGIN, struct.pack(">I", size))
    def commit(self): self._empty(COMMIT)
    def abort(self): self._empty(ABORT)
    def reboot(self): self._empty(REBOOT)

    def write(self, offset, data):
        if not 1 <= len(data) <= MAX_CHUNK:
            raise UpdateError("OTA chunk exceeds the v1 payload limit")
        response = self.control(WRITE, struct.pack(">I", offset) + data)
        if len(response) != 4:
            raise UpdateError("invalid OTA WRITE response length")
        return int.from_bytes(response, "big")


def image_version(image):
    """Read the ESP-IDF application descriptor; device still validates the image."""
    image.seek(0)
    prefix = image.read(80)
    image.seek(0)
    if len(prefix) != 80 or prefix[0] != 0xe9 or struct.unpack_from("<I", prefix, 32)[0] != 0xabcd5432:
        raise UpdateError("not an ESP-IDF application image with an app descriptor")
    raw = prefix[48:80].split(b"\0", 1)[0]
    try:
        version = raw.decode("utf-8")
    except UnicodeDecodeError as error:
        raise UpdateError("invalid application version") from error
    if not version: raise UpdateError("application descriptor omits version")
    return version


def upload(client, image, size, *, chunk_size=MAX_CHUNK):
    before = client.query()
    if not 0 < size <= min(before.capacity, 0xffffffff):
        raise UpdateError("image size exceeds inactive slot capacity or is empty")
    if not 1 <= chunk_size <= MAX_CHUNK:
        raise UpdateError("invalid OTA chunk size")
    begun = False
    try:
        client.begin(size)
        begun = True
        offset = 0
        while offset < size:
            requested = min(chunk_size, size - offset)
            data = image.read(requested)
            if not data or len(data) > requested:
                raise UpdateError("image length changed during upload")
            next_offset = client.write(offset, data)
            if next_offset != offset + len(data):
                raise UpdateError("device returned an unexpected next offset")
            offset = next_offset
        if image.read(1):
            raise UpdateError("image contains bytes beyond declared size")
        client.commit()
    except BaseException as error:
        if begun:
            try:
                client.abort()
            except Exception as cleanup:
                error.add_note(f"OTA abort could not be acknowledged: {cleanup}; closing the connection")
        raise
    return before


def verify_updated(before, after, expected_version):
    if after.running_address != before.update_address or after.running_address == before.running_address:
        raise UpdateError("device did not boot the previously inactive slot")
    if after.version != expected_version:
        raise UpdateError(f"running version {after.version!r} differs from image {expected_version!r}")
    if not after.confirmed:
        raise UpdateError("running image has not passed startup confirmation")


def wait_updated(host, credential, before, version, *, port=3260, timeout=45.0):
    deadline = time.monotonic() + timeout
    last_error = None
    while time.monotonic() < deadline:
        try:
            with Client(host, credential, port=port,
                        timeout=min(5.0, max(0.1, deadline - time.monotonic()))) as client:
                after = client.query()
        except (UpdateError, OSError) as error:
            last_error = error
            time.sleep(min(0.25, max(0, deadline - time.monotonic())))
            continue
        verify_updated(before, after, version)
        return after
    raise UpdateError(f"same-device OTA reconnect timed out: {last_error}")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("host")
    parser.add_argument("image", type=Path, nargs="?")
    parser.add_argument("--credential", type=Path, required=True)
    parser.add_argument("--port", type=int, default=3260)
    actions = parser.add_mutually_exclusive_group()
    actions.add_argument("--query", action="store_true")
    actions.add_argument("--reboot", action="store_true", help="reboot an already committed image")
    parser.add_argument("--chunk-size", type=int, default=MAX_CHUNK)
    parser.add_argument("--reconnect-timeout", type=float, default=45.0)
    args = parser.parse_args(argv)
    if (args.image is None) != (args.query or args.reboot):
        parser.error("supply an image, or select --query / --reboot")
    if not 1 <= args.chunk_size <= MAX_CHUNK or not 1 <= args.port <= 65535 or args.reconnect_timeout <= 0:
        parser.error("invalid chunk size, port or reconnect timeout")
    credential = load_credential(args.credential)
    if args.query or args.reboot:
        with Client(args.host, credential, port=args.port) as client:
            if args.reboot:
                client.reboot()
                print("Reboot acknowledged; verify the running slot and version with --query.")
            else:
                print(json.dumps(dict(device_id=credential.device_id, **asdict(client.query())), indent=2))
        return 0
    if not args.image.is_file(): raise UpdateError("image must be a regular file")
    with args.image.open("rb") as image:
        import os
        size = os.fstat(image.fileno()).st_size
        version = image_version(image)
        with Client(args.host, credential, port=args.port) as client:
            before = upload(client, image, size, chunk_size=args.chunk_size)
            client.reboot()
    after = wait_updated(args.host, credential, before, version,
        port=args.port, timeout=args.reconnect_timeout)
    print(json.dumps(dict(device_id=credential.device_id, before=asdict(before), after=asdict(after)), indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (UpdateError, CredentialError, OSError) as error:
        print(f"AirDAP network update failed: {error}", file=sys.stderr)
        for note in getattr(error, "__notes__", ()):
            print(note, file=sys.stderr)
        raise SystemExit(1)
