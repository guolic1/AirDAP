#!/usr/bin/env python3
"""Verify an authenticated AirDAP DAP TCP session and DAP_Info."""

from __future__ import annotations

import argparse
import secrets
import socket
import ssl
import struct
import sys
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Protocol

from airdap_network_credential import CredentialError
from airdap_network_credential import NetworkCredential
from airdap_network_credential import load_credential


DEFAULT_PORT = 3260
DEFAULT_TIMEOUT_SECONDS = 5.0
EXPECTED_TLS_VERSION = "TLSv1.3"
EXPECTED_CIPHER = "TLS_AES_128_GCM_SHA256"
FRAME_MAGIC = b"ADAP"
FRAME_VERSION = 1
FRAME_MAX_PAYLOAD = 4096
FRAME_HEADER = struct.Struct(">4sBBHIIHH")
HELLO_FIXED_SIZE = 36
AUTH_RESPONSE_SIZE = 36
SESSION_TOKEN_SIZE = 32
UINT32_MAX = (1 << 32) - 1

TYPE_HELLO = 1
TYPE_AUTH = 2
TYPE_DAP_REQUEST = 3
TYPE_DAP_RESPONSE = 4
TYPE_KEEPALIVE = 7
TYPE_ERROR = 8

DAP_INFO = 0x00
DAP_INFO_PRODUCT_FIRMWARE_VERSION = 0x09


class ProbeError(RuntimeError):
    """Raised when the authenticated DAP contract is not satisfied."""


class SocketIO(Protocol):
    def recv(self, size: int) -> bytes: ...

    def sendall(self, data: bytes) -> None: ...


@dataclass(frozen=True)
class ProbeResult:
    tls_version: str
    cipher: str
    device_id: str
    uuid: bytes
    capabilities: int
    firmware_version: str
    owner_session_id: int
    session_token: bytes


def _encode_frame(
    message_type: int,
    session_id: int,
    sequence: int,
    payload: bytes = b"",
) -> bytes:
    if not 1 <= session_id <= UINT32_MAX or not 1 <= sequence <= UINT32_MAX:
        raise ProbeError("session and sequence must be non-zero uint32 values")
    if len(payload) > FRAME_MAX_PAYLOAD:
        raise ProbeError("AirDAP payload exceeds the v1 limit")
    return FRAME_HEADER.pack(
        FRAME_MAGIC,
        FRAME_VERSION,
        message_type,
        0,
        session_id,
        sequence,
        len(payload),
        0,
    ) + payload


def _read_exact(connection: SocketIO, size: int) -> bytes:
    output = bytearray()
    while len(output) < size:
        chunk = connection.recv(size - len(output))
        if not chunk:
            raise ProbeError("AirDAP connection closed during a frame")
        output.extend(chunk)
    return bytes(output)


def _read_response(
    connection: SocketIO,
    *,
    expected_type: int,
    session_id: int,
    sequence: int,
) -> bytes:
    header = FRAME_HEADER.unpack(_read_exact(connection, FRAME_HEADER.size))
    magic, version, message_type, flags, frame_session, frame_sequence, size, reserved = (
        header
    )
    if magic != FRAME_MAGIC:
        raise ProbeError("invalid AirDAP frame magic")
    if version != FRAME_VERSION:
        raise ProbeError(f"unsupported AirDAP protocol version: {version}")
    if flags != 0 or reserved != 0:
        raise ProbeError("invalid AirDAP v1 flags or reserved field")
    if frame_session != session_id or frame_sequence != sequence:
        raise ProbeError("AirDAP response session or sequence mismatch")
    if size > FRAME_MAX_PAYLOAD:
        raise ProbeError("AirDAP response payload exceeds the v1 limit")
    payload = _read_exact(connection, size)
    if message_type == TYPE_ERROR:
        if len(payload) != 2:
            raise ProbeError("malformed AirDAP ERROR payload")
        raise ProbeError(f"AirDAP error 0x{int.from_bytes(payload, 'big'):04X}")
    if message_type != expected_type:
        raise ProbeError(
            f"unexpected AirDAP response type: {message_type}, expected {expected_type}"
        )
    return payload


def _parse_hello(payload: bytes, expected_device_id: str) -> tuple[bytes, int, str]:
    if len(payload) <= HELLO_FIXED_SIZE:
        raise ProbeError("malformed AirDAP HELLO payload")
    uuid = payload[:16]
    capabilities = int.from_bytes(payload[16:20], "big")
    try:
        device_id = payload[20:36].decode("ascii")
        firmware_version = payload[HELLO_FIXED_SIZE:].decode("utf-8")
    except UnicodeDecodeError as error:
        raise ProbeError("invalid text in AirDAP HELLO payload") from error
    if device_id != expected_device_id:
        raise ProbeError(
            f"HELLO device mismatch: received {device_id}, expected {expected_device_id}"
        )
    if not firmware_version:
        raise ProbeError("AirDAP HELLO omitted the firmware version")
    return uuid, capabilities, firmware_version


def _parse_auth(payload: bytes) -> tuple[int, bytes]:
    if len(payload) != AUTH_RESPONSE_SIZE:
        raise ProbeError("malformed AirDAP AUTH payload")
    owner_session_id = int.from_bytes(payload[:4], "big")
    if owner_session_id == 0:
        raise ProbeError("AirDAP AUTH returned a reserved owner session")
    return owner_session_id, payload[4:]


def _parse_dap_info(payload: bytes) -> str:
    if len(payload) < 2 or payload[0] != DAP_INFO:
        raise ProbeError("malformed CMSIS-DAP DAP_Info response")
    value_size = payload[1]
    if len(payload) != 2 + value_size or value_size == 0:
        raise ProbeError("invalid CMSIS-DAP DAP_Info length")
    value = payload[2:]
    if value[-1] != 0:
        raise ProbeError("CMSIS-DAP firmware version is not NUL terminated")
    try:
        return value[:-1].decode("utf-8")
    except UnicodeDecodeError as error:
        raise ProbeError("invalid CMSIS-DAP firmware version text") from error


def probe_dap(
    host: str,
    port: int,
    credential: NetworkCredential,
    *,
    timeout: float = DEFAULT_TIMEOUT_SECONDS,
) -> ProbeResult:
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    context.minimum_version = ssl.TLSVersion.TLSv1_3
    context.maximum_version = ssl.TLSVersion.TLSv1_3
    context.check_hostname = False
    context.verify_mode = ssl.CERT_NONE

    def psk_callback(hint: str | None) -> tuple[str, bytes]:
        if hint not in (None, credential.identity):
            raise ssl.SSLError("AirDAP PSK identity hint mismatch")
        return credential.identity, credential.psk

    try:
        context.set_psk_client_callback(psk_callback)
    except AttributeError as error:
        raise ProbeError("Python 3.13 or newer with TLS-PSK support is required") from error

    session_id = secrets.randbelow(UINT32_MAX) + 1
    try:
        with socket.create_connection((host, port), timeout=timeout) as raw_socket:
            with context.wrap_socket(raw_socket, server_hostname=None) as tls_socket:
                tls_version = tls_socket.version()
                cipher_details = tls_socket.cipher()
                if tls_version != EXPECTED_TLS_VERSION:
                    raise ProbeError(f"unexpected TLS version: {tls_version}")
                cipher = cipher_details[0] if cipher_details is not None else ""
                if cipher != EXPECTED_CIPHER:
                    raise ProbeError(f"unexpected TLS cipher: {cipher or 'none'}")

                tls_socket.sendall(_encode_frame(TYPE_HELLO, session_id, 1))
                hello = _read_response(
                    tls_socket,
                    expected_type=TYPE_HELLO,
                    session_id=session_id,
                    sequence=1,
                )
                uuid, capabilities, hello_firmware = _parse_hello(
                    hello, credential.device_id
                )

                tls_socket.sendall(_encode_frame(TYPE_AUTH, session_id, 2))
                auth = _read_response(
                    tls_socket,
                    expected_type=TYPE_AUTH,
                    session_id=session_id,
                    sequence=2,
                )
                owner_session_id, session_token = _parse_auth(auth)

                dap_request = bytes((DAP_INFO, DAP_INFO_PRODUCT_FIRMWARE_VERSION))
                tls_socket.sendall(
                    _encode_frame(TYPE_DAP_REQUEST, session_id, 3, dap_request)
                )
                dap_response = _read_response(
                    tls_socket,
                    expected_type=TYPE_DAP_RESPONSE,
                    session_id=session_id,
                    sequence=3,
                )
                dap_firmware = _parse_dap_info(dap_response)
                if dap_firmware != hello_firmware:
                    raise ProbeError(
                        "HELLO and CMSIS-DAP report different firmware versions"
                    )

                tls_socket.sendall(_encode_frame(TYPE_KEEPALIVE, session_id, 4))
                keepalive = _read_response(
                    tls_socket,
                    expected_type=TYPE_KEEPALIVE,
                    session_id=session_id,
                    sequence=4,
                )
                if keepalive:
                    raise ProbeError("AirDAP KEEPALIVE response must be empty")
    except ProbeError:
        raise
    except (OSError, ssl.SSLError) as error:
        raise ProbeError(f"authenticated DAP probe failed: {error}") from error

    return ProbeResult(
        tls_version=tls_version,
        cipher=cipher,
        device_id=credential.device_id,
        uuid=uuid,
        capabilities=capabilities,
        firmware_version=hello_firmware,
        owner_session_id=owner_session_id,
        session_token=session_token,
    )


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Verify an authenticated AirDAP DAP TCP session."
    )
    parser.add_argument("host")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--credential", required=True, type=Path)
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT_SECONDS)
    args = parser.parse_args(argv)
    if not 1 <= args.port <= 65535 or args.timeout <= 0:
        raise ProbeError("port and timeout must be positive and in range")
    try:
        credential = load_credential(args.credential)
    except CredentialError as error:
        raise ProbeError(str(error)) from error
    result = probe_dap(
        args.host,
        args.port,
        credential,
        timeout=args.timeout,
    )
    print(
        f"Authenticated DAP {result.device_id}: "
        f"{result.tls_version} / {result.cipher}; "
        f"fw={result.firmware_version}; "
        f"uuid={result.uuid.hex()}; "
        f"cap=0x{result.capabilities:08X}; "
        f"owner_session={result.owner_session_id}"
    )
    return 0


def _run_cli(argv: Sequence[str] | None = None) -> int:
    try:
        return main(argv)
    except ProbeError as error:
        print(f"airdap-dap-probe: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(_run_cli())
