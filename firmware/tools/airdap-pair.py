#!/usr/bin/env python3
"""Rotate one AirDAP network PSK through its Security 2 BLE endpoint."""

from __future__ import annotations

import argparse
import asyncio
import importlib.util
import os
import secrets
import sys
from collections.abc import Sequence
from pathlib import Path
from types import ModuleType
from typing import Any

from airdap_network_credential import CredentialError
from airdap_network_credential import NetworkCredential
from airdap_network_credential import load_or_create_credential


FIRMWARE_DIR = Path(__file__).resolve().parents[1]
CONFIGURED_IDF_PATH_FILE = FIRMWARE_DIR / ".airdap-env" / "idf-path.txt"
ESP_PROV_DIR = (
    FIRMWARE_DIR
    / "managed_components"
    / "espressif__network_provisioning"
    / "tool"
    / "esp_prov"
)
ESP_PROV_SCRIPT = ESP_PROV_DIR / "esp_prov.py"
PAIRING_ENDPOINT = "airdap-pair"
PAIRING_REQUEST_VERSION = 1
PUBLIC_SEC2_USERNAME = "wifiprov"
PUBLIC_SEC2_POP = "abcd1234"


PairingError = CredentialError


def _validated_idf_path(candidate: str) -> Path:
    idf_path = Path(candidate).expanduser().resolve()
    required = idf_path / "components" / "protocomm" / "python" / "session_pb2.py"
    if not required.is_file():
        raise PairingError(f"configured ESP-IDF path {idf_path} is incomplete")
    return idf_path


def resolve_idf_path() -> Path:
    environment_path = os.environ.get("IDF_PATH", "").strip()
    if environment_path:
        return _validated_idf_path(environment_path)
    try:
        lines = CONFIGURED_IDF_PATH_FILE.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise PairingError(
            "ESP-IDF is not configured; run `python firmware/tools/setup.py`"
        ) from error
    if not lines or not lines[0].strip():
        raise PairingError("configured ESP-IDF path is empty")
    return _validated_idf_path(lines[0].strip())


def load_esp_prov(idf_path: Path) -> ModuleType:
    if not ESP_PROV_SCRIPT.is_file():
        raise PairingError(
            "the provisioning client is not downloaded; run `idf.py reconfigure` "
            "in firmware/ first"
        )
    protocomm_python = idf_path / "components" / "protocomm" / "python"
    for import_path in (ESP_PROV_DIR, protocomm_python):
        path_text = str(import_path)
        if path_text not in sys.path:
            sys.path.insert(0, path_text)
    spec = importlib.util.spec_from_file_location(
        "airdap_upstream_esp_prov", ESP_PROV_SCRIPT
    )
    if spec is None or spec.loader is None:
        raise PairingError("cannot load the provisioning client")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _latin1_bytes(value: Any) -> bytes:
    if isinstance(value, bytes):
        return value
    if isinstance(value, str):
        return value.encode("latin-1")
    raise PairingError("provisioning endpoint returned an invalid response type")


async def pair_device(
    credential: NetworkCredential,
    esp_prov: Any,
    *,
    ble_adapter: str = "hci0",
) -> bytes:
    transport = await esp_prov.get_transport(
        "ble", credential.device_id, ble_adapter
    )
    if transport is None:
        raise PairingError("failed to connect to the AirDAP BLE window")
    try:
        patch_version = await esp_prov.get_sec_patch_ver(transport)
        security = esp_prov.get_security(
            2,
            patch_version,
            PUBLIC_SEC2_USERNAME,
            PUBLIC_SEC2_POP,
        )
        if security is None or not await esp_prov.establish_session(
            transport, security
        ):
            raise PairingError("Security 2 session establishment failed")

        request = bytes([PAIRING_REQUEST_VERSION]) + credential.psk
        encrypted_request = security.encrypt_data(request).decode("latin-1")
        encrypted_response = await transport.send_data(
            PAIRING_ENDPOINT, encrypted_request
        )
        fingerprint = security.decrypt_data(_latin1_bytes(encrypted_response))
        if len(fingerprint) != len(credential.fingerprint) or not secrets.compare_digest(
            fingerprint, credential.fingerprint
        ):
            raise PairingError("device returned an unexpected credential fingerprint")
        return fingerprint
    except PairingError:
        raise
    except Exception as error:
        raise PairingError(str(error) or type(error).__name__) from error
    finally:
        try:
            await transport.disconnect()
        except Exception as error:
            raise PairingError(
                f"failed to disconnect from the AirDAP BLE window: {error}"
            ) from error


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Create or reuse one protected 256-bit credential and rotate it "
            "through an AirDAP Security 2 BLE window."
        )
    )
    parser.add_argument(
        "--device",
        required=True,
        help="AirDAP device ID, for example ADP-001122334455",
    )
    parser.add_argument(
        "--credential",
        required=True,
        type=Path,
        help="explicit private credential file (created as mode 0600 on POSIX)",
    )
    parser.add_argument("--ble-adapter", default="hci0")
    args = parser.parse_args(argv)

    credential = load_or_create_credential(args.credential, args.device)
    idf_path = resolve_idf_path()
    esp_prov = load_esp_prov(idf_path)
    asyncio.run(
        pair_device(credential, esp_prov, ble_adapter=args.ble_adapter)
    )
    print(
        f"Paired {credential.device_id}; "
        f"fingerprint={credential.fingerprint.hex()}"
    )
    return 0


def _run_cli(argv: Sequence[str] | None = None) -> int:
    try:
        return main(argv)
    except PairingError as error:
        print(f"airdap-pair: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(_run_cli())
