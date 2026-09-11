#!/usr/bin/env python3
"""Discover an AirDAP and run its interactive BLE provisioning client."""

from __future__ import annotations

import argparse
import asyncio
import os
import re
import subprocess
import sys
from collections.abc import Callable, Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any


FIRMWARE_DIR = Path(__file__).resolve().parents[1]
CONFIGURED_IDF_PATH_FILE = FIRMWARE_DIR / ".airdap-env" / "idf-path.txt"
ESP_PROV_SCRIPT = (
    FIRMWARE_DIR
    / "managed_components"
    / "espressif__network_provisioning"
    / "tool"
    / "esp_prov"
    / "esp_prov.py"
)

AIRDAP_DEVICE_NAME_PATTERN = re.compile(r"^ADP-[0-9A-F]{12}$")
AIRDAP_PROVISIONING_SERVICE_UUID = "1775244d-6b43-439b-877c-060f2d9bed07"
PUBLIC_SEC2_USERNAME = "wifiprov"
PUBLIC_SEC2_POP = "abcd1234"
DEFAULT_SCAN_TIMEOUT_SECONDS = 10.0
ESP_PROV_LAUNCHER = Path(__file__).with_name("airdap_esp_prov.py")


class ProvisioningError(RuntimeError):
    """Raised when the AirDAP provisioning workflow cannot proceed."""


@dataclass(frozen=True)
class DiscoveredAirDap:
    name: str
    address: str


async def discover_airdap_devices(
    scanner: Any,
    *,
    timeout: float = DEFAULT_SCAN_TIMEOUT_SECONDS,
) -> list[DiscoveredAirDap]:
    discovery = await scanner.discover(timeout=timeout, return_adv=True)
    expected_uuid = AIRDAP_PROVISIONING_SERVICE_UUID.lower()
    devices: dict[tuple[str, str], DiscoveredAirDap] = {}

    for device, advertisement in discovery.values():
        name = getattr(advertisement, "local_name", None) or getattr(
            device, "name", None
        )
        address = getattr(device, "address", None)
        service_uuids = getattr(advertisement, "service_uuids", None) or []
        if (
            not isinstance(name, str)
            or AIRDAP_DEVICE_NAME_PATTERN.fullmatch(name) is None
            or not isinstance(address, str)
            or expected_uuid not in {str(uuid).lower() for uuid in service_uuids}
        ):
            continue
        candidate = DiscoveredAirDap(name=name, address=address)
        devices[(name, address)] = candidate

    return sorted(devices.values(), key=lambda device: (device.name, device.address))


def select_airdap_device(
    devices: Sequence[DiscoveredAirDap],
    *,
    input_fn: Callable[[str], str] = input,
    output_fn: Callable[[str], None] = print,
) -> DiscoveredAirDap:
    if not devices:
        raise ProvisioningError(
            "no provisioning AirDAP found; hold BOOT_KEY for three seconds, "
            "release it, and retry within the 120-second BLE window"
        )
    if len(devices) == 1:
        return devices[0]

    output_fn("Multiple provisioning AirDAP devices found:")
    for index, device in enumerate(devices, start=1):
        output_fn(f"[{index}] {device.name} ({device.address})")

    while True:
        try:
            selection = int(input_fn("Select AirDAP by number: "))
        except ValueError:
            output_fn("Invalid selection; try again.")
            continue
        if 1 <= selection <= len(devices):
            return devices[selection - 1]
        output_fn("Invalid selection; try again.")


def _validated_idf_path(candidate: str) -> Path:
    idf_path = Path(candidate).expanduser().resolve()
    required_files = (
        idf_path / "tools" / "idf.py",
        idf_path / "components" / "protocomm" / "python" / "session_pb2.py",
    )
    missing = [str(path) for path in required_files if not path.is_file()]
    if missing:
        raise ProvisioningError(
            f"configured ESP-IDF path {idf_path} is incomplete; missing: "
            + ", ".join(missing)
        )
    return idf_path


def resolve_idf_path(
    environ: Mapping[str, str] = os.environ,
    *,
    config_file: Path = CONFIGURED_IDF_PATH_FILE,
) -> Path:
    environment_path = environ.get("IDF_PATH", "").strip()
    if environment_path:
        return _validated_idf_path(environment_path)

    try:
        lines = config_file.read_text(encoding="utf-8").splitlines()
    except FileNotFoundError as error:
        raise ProvisioningError(
            "ESP-IDF is not configured; run `python firmware/tools/setup.py` from "
            "the repository root, or `python tools/setup.py` from firmware/"
        ) from error
    except OSError as error:
        raise ProvisioningError(
            f"cannot read ESP-IDF configuration {config_file}: {error}"
        ) from error

    if not lines or not lines[0].strip():
        raise ProvisioningError(
            f"ESP-IDF configuration {config_file} does not contain a path"
        )
    if len(lines) > 2 or (len(lines) == 2 and lines[1] not in {"managed", "external"}):
        raise ProvisioningError(
            f"ESP-IDF configuration {config_file} has an invalid format; "
            "rerun `python firmware/tools/setup.py` from the repository root, "
            "or `python tools/setup.py` from firmware/"
        )
    return _validated_idf_path(lines[0].strip())


def build_esp_prov_command(
    device_name: str,
    *,
    python_executable: str = sys.executable,
    esp_prov_script: Path = ESP_PROV_SCRIPT,
) -> list[str]:
    # These Security 2 credentials are deliberately public firmware constants.
    # The private Wi-Fi passphrase is omitted so esp_prov prompts without echo.
    return [
        python_executable,
        str(ESP_PROV_LAUNCHER),
        str(esp_prov_script),
        "--transport",
        "ble",
        "--service_name",
        device_name,
        "--sec_ver",
        "2",
        "--sec2_username",
        PUBLIC_SEC2_USERNAME,
        "--sec2_pwd",
        PUBLIC_SEC2_POP,
    ]


def run_provisioning_client(
    device_name: str,
    idf_path: Path,
    *,
    esp_prov_script: Path = ESP_PROV_SCRIPT,
    runner: Callable[..., Any] = subprocess.run,
) -> int:
    if not esp_prov_script.is_file():
        raise ProvisioningError(
            "the ESP-IDF provisioning client is not downloaded; run "
            "`idf.py reconfigure` in firmware/ first"
        )

    environment = os.environ.copy()
    environment["IDF_PATH"] = str(idf_path)
    try:
        result = runner(
            build_esp_prov_command(device_name, esp_prov_script=esp_prov_script),
            env=environment,
            check=False,
        )
    except OSError as error:
        raise ProvisioningError(f"cannot start the provisioning client: {error}") from error
    return int(result.returncode)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(
        description=(
            "Discover an AirDAP BLE provisioning window and configure Wi-Fi "
            "with the firmware's public Security 2 credential."
        )
    )
    parser.parse_args(argv)

    idf_path = resolve_idf_path()
    if not ESP_PROV_SCRIPT.is_file():
        raise ProvisioningError(
            "the ESP-IDF provisioning client is not downloaded; run "
            "`idf.py reconfigure` in firmware/ first"
        )
    try:
        from bleak import BleakScanner
    except ImportError as error:
        raise ProvisioningError(
            "Python package `bleak` is missing; run `uv sync` from the repository root"
        ) from error

    print("Scanning for AirDAP provisioning windows...")
    try:
        candidates = asyncio.run(discover_airdap_devices(BleakScanner))
    except Exception as error:
        raise ProvisioningError(str(error) or type(error).__name__) from error
    selected = select_airdap_device(candidates)
    print(f"Using {selected.name} ({selected.address})")
    return run_provisioning_client(selected.name, idf_path)


def _run_cli(argv: Sequence[str] | None = None) -> int:
    try:
        return main(argv)
    except ProvisioningError as error:
        print(f"airdap-provision: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(_run_cli())
