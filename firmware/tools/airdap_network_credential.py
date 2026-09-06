"""Explicitly protected development credential files for AirDAP network PSK."""

from __future__ import annotations

import hashlib
import json
import os
import re
import secrets
from dataclasses import dataclass
from pathlib import Path
from typing import Any


DEVICE_ID_PATTERN = re.compile(r"^ADP-[0-9A-F]{12}$")
PSK_SIZE = 32
FINGERPRINT_SIZE = 32
CREDENTIAL_VERSION = 1
FINGERPRINT_LABEL = b"AirDAP network PSK v1"


class CredentialError(RuntimeError):
    """Raised when a credential file cannot be safely created or loaded."""


@dataclass(frozen=True)
class NetworkCredential:
    device_id: str
    identity: str
    psk: bytes
    fingerprint: bytes

    @classmethod
    def create(cls, device_id: str, psk: bytes) -> "NetworkCredential":
        if DEVICE_ID_PATTERN.fullmatch(device_id) is None:
            raise CredentialError("device ID must match ADP- followed by 12 hex digits")
        if len(psk) != PSK_SIZE:
            raise CredentialError("network PSK must contain exactly 32 bytes")
        identity = f"AIRDAP:{device_id}"
        fingerprint = hashlib.sha256(
            FINGERPRINT_LABEL + identity.encode("ascii") + psk
        ).digest()
        return cls(
            device_id=device_id,
            identity=identity,
            psk=psk,
            fingerprint=fingerprint,
        )

    def to_json(self) -> str:
        return json.dumps(
            {
                "version": CREDENTIAL_VERSION,
                "device_id": self.device_id,
                "identity": self.identity,
                "psk": self.psk.hex(),
                "fingerprint": self.fingerprint.hex(),
            },
            indent=2,
            sort_keys=True,
        ) + "\n"

    @classmethod
    def from_json(cls, value: str) -> "NetworkCredential":
        try:
            record: Any = json.loads(value)
        except (json.JSONDecodeError, TypeError) as error:
            raise CredentialError("credential file is not valid JSON") from error
        if not isinstance(record, dict) or record.get("version") != CREDENTIAL_VERSION:
            raise CredentialError("credential file version is unsupported")
        try:
            device_id = record["device_id"]
            identity = record["identity"]
            psk = bytes.fromhex(record["psk"])
            stored_fingerprint = bytes.fromhex(record["fingerprint"])
        except (KeyError, TypeError, ValueError) as error:
            raise CredentialError("credential file fields are invalid") from error
        if not isinstance(device_id, str) or not isinstance(identity, str):
            raise CredentialError("credential file identity fields are invalid")
        candidate = cls.create(device_id, psk)
        if identity != candidate.identity or not secrets.compare_digest(
            stored_fingerprint, candidate.fingerprint
        ):
            raise CredentialError("credential file integrity check failed")
        return candidate


def _validate_private_permissions(path: Path) -> None:
    if os.name != "nt" and path.stat().st_mode & 0o077:
        raise CredentialError(
            f"credential file {path} permissions expose it to group/other users; "
            "use mode 0600"
        )


def _create_private_file(path: Path, credential: NetworkCredential) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    flags = os.O_WRONLY | os.O_CREAT | os.O_EXCL
    if hasattr(os, "O_BINARY"):
        flags |= os.O_BINARY
    try:
        descriptor = os.open(path, flags, 0o600)
    except FileExistsError:
        raise
    except OSError as error:
        raise CredentialError(f"cannot create credential file {path}: {error}") from error
    try:
        if os.name != "nt":
            os.fchmod(descriptor, 0o600)
        payload = credential.to_json().encode("utf-8")
        offset = 0
        while offset < len(payload):
            written = os.write(descriptor, payload[offset:])
            if written == 0:
                raise OSError("credential write made no progress")
            offset += written
        os.fsync(descriptor)
    except OSError as error:
        try:
            opened = os.fstat(descriptor)
            current = os.stat(path, follow_symlinks=False)
            if (opened.st_dev, opened.st_ino) == (current.st_dev, current.st_ino):
                path.unlink()
        except OSError:
            pass
        raise CredentialError(f"cannot write credential file {path}: {error}") from error
    finally:
        os.close(descriptor)


def load_credential(path: Path) -> NetworkCredential:
    try:
        _validate_private_permissions(path)
        value = path.read_text(encoding="utf-8")
    except FileNotFoundError as error:
        raise CredentialError(f"credential file {path} does not exist") from error
    except (OSError, UnicodeError) as error:
        raise CredentialError(f"cannot read credential file {path}: {error}") from error
    return NetworkCredential.from_json(value)


def load_or_create_credential(path: Path, device_id: str) -> NetworkCredential:
    try:
        credential = load_credential(path)
    except CredentialError as error:
        if path.exists():
            raise
        credential = NetworkCredential.create(device_id, secrets.token_bytes(PSK_SIZE))
        try:
            _create_private_file(path, credential)
        except FileExistsError:
            credential = load_credential(path)
    if credential.device_id != device_id:
        raise CredentialError(
            f"credential file {path} belongs to {credential.device_id}, not {device_id}"
        )
    return credential
