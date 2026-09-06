#!/usr/bin/env python3
"""Verify the AirDAP TLS 1.3 PSK-DHE handshake contract."""

from __future__ import annotations

import argparse
import socket
import ssl
import sys
from collections.abc import Sequence
from dataclasses import dataclass
from pathlib import Path

from airdap_network_credential import CredentialError
from airdap_network_credential import NetworkCredential
from airdap_network_credential import load_credential


EXPECTED_TLS_VERSION = "TLSv1.3"
EXPECTED_CIPHER = "TLS_AES_128_GCM_SHA256"
DEFAULT_PORT = 3260
DEFAULT_TIMEOUT_SECONDS = 5.0


class ProbeError(RuntimeError):
    """Raised when the TLS authentication contract is not satisfied."""


@dataclass(frozen=True)
class ProbeResult:
    version: str
    cipher: str


def probe_tls(
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

    try:
        with socket.create_connection((host, port), timeout=timeout) as raw_socket:
            with context.wrap_socket(raw_socket, server_hostname=None) as tls_socket:
                version = tls_socket.version()
                cipher_details = tls_socket.cipher()
    except (OSError, ssl.SSLError) as error:
        raise ProbeError(f"TLS handshake failed: {error}") from error
    if version != EXPECTED_TLS_VERSION:
        raise ProbeError(f"unexpected TLS version: {version}")
    cipher = cipher_details[0] if cipher_details is not None else ""
    if cipher != EXPECTED_CIPHER:
        raise ProbeError(f"unexpected TLS cipher: {cipher or 'none'}")
    return ProbeResult(version=version, cipher=cipher)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description="Verify an AirDAP TLS 1.3 PSK-DHE handshake."
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
    result = probe_tls(
        args.host,
        args.port,
        credential,
        timeout=args.timeout,
    )
    print(
        f"Authenticated {credential.device_id}: "
        f"{result.version} / {result.cipher}; "
        f"fingerprint={credential.fingerprint.hex()}"
    )
    return 0


def _run_cli(argv: Sequence[str] | None = None) -> int:
    try:
        return main(argv)
    except ProbeError as error:
        print(f"airdap-tls-probe: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(_run_cli())
