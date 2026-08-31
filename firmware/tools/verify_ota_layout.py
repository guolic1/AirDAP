#!/usr/bin/env python3
"""Verify AirDAP's persistent 8 MiB A/B application layout contract."""

from __future__ import annotations

import argparse
import csv
import re
import sys
from collections.abc import Mapping, Sequence
from pathlib import Path
from typing import NamedTuple


FLASH_SIZE = 8 * 1024 * 1024
REQUIRED_PARTITIONS = {
    "nvs": ("data", "nvs", 0x9000, 0x6000),
    "otadata": ("data", "ota", 0xF000, 0x2000),
    "phy_init": ("data", "phy", 0x11000, 0x1000),
    "ota_0": ("app", "ota_0", 0x20000, 0x3F0000),
    "ota_1": ("app", "ota_1", 0x410000, 0x3F0000),
}

_UNSET_CONFIG = re.compile(r"^# (CONFIG_[A-Z0-9_]+) is not set$")


class VerificationError(RuntimeError):
    """Raised when checked-in OTA configuration violates the contract."""


class Partition(NamedTuple):
    name: str
    type: str
    subtype: str
    offset: int
    size: int


def parse_size(value: str) -> int:
    """Parse ESP-IDF partition-table integer values and binary K/M suffixes."""
    normalized = value.strip().upper()
    multiplier = 1
    if normalized.endswith("K"):
        normalized = normalized[:-1]
        multiplier = 1024
    elif normalized.endswith("M"):
        normalized = normalized[:-1]
        multiplier = 1024 * 1024
    try:
        result = int(normalized, 0) * multiplier
    except ValueError as error:
        raise VerificationError(f"invalid partition integer {value!r}") from error
    if result < 0:
        raise VerificationError(f"partition integer must be non-negative: {value!r}")
    return result


def parse_partition_csv(text: str) -> list[Partition]:
    """Parse an explicit-offset ESP-IDF partition CSV file."""
    rows: list[Partition] = []
    for fields in csv.reader(text.splitlines(), skipinitialspace=True):
        if not fields or not fields[0].strip() or fields[0].lstrip().startswith("#"):
            continue
        if len(fields) < 5:
            raise VerificationError(
                f"partition row needs at least five fields: {fields!r}"
            )
        name, type_name, subtype, offset, size = (
            field.strip() for field in fields[:5]
        )
        if not all((name, type_name, subtype, offset, size)):
            raise VerificationError(
                f"partition {name or '<unnamed>'} must use explicit fields and offset"
            )
        rows.append(
            Partition(
                name=name,
                type=type_name,
                subtype=subtype,
                offset=parse_size(offset),
                size=parse_size(size),
            )
        )
    return rows


def parse_sdkconfig(text: str) -> dict[str, str | None]:
    values: dict[str, str | None] = {}
    for raw_line in text.splitlines():
        line = raw_line.strip()
        unset = _UNSET_CONFIG.match(line)
        if unset is not None:
            values[unset.group(1)] = None
        elif line.startswith("CONFIG_") and "=" in line:
            name, value = line.split("=", 1)
            values[name] = value
    return values


def _require_config(
    config: Mapping[str, str | None],
    name: str,
    expected: str,
    description: str,
) -> None:
    actual = config.get(name)
    if actual != expected:
        raise VerificationError(
            f"{description} must be {expected}, found {actual!r} ({name})"
        )


def validate_config(config: Mapping[str, str | None]) -> None:
    _require_config(
        config,
        "CONFIG_ESPTOOLPY_FLASHSIZE_8MB",
        "y",
        "configured flash size selection for the 8 MiB module",
    )
    _require_config(
        config,
        "CONFIG_ESPTOOLPY_FLASHSIZE",
        '"8MB"',
        "configured flash size string",
    )
    _require_config(
        config,
        "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE",
        "y",
        "OTA rollback",
    )
    _require_config(
        config,
        "CONFIG_PARTITION_TABLE_CUSTOM",
        "y",
        "custom partition table",
    )
    _require_config(
        config,
        "CONFIG_PARTITION_TABLE_CUSTOM_FILENAME",
        '"partitions.csv"',
        "AirDAP partition table filename",
    )


def validate_layout(partitions: Sequence[Partition]) -> None:
    by_name: dict[str, Partition] = {}
    for partition in partitions:
        if partition.name in by_name:
            raise VerificationError(f"duplicate partition name: {partition.name}")
        if partition.size == 0:
            raise VerificationError(f"partition {partition.name} must not be empty")
        by_name[partition.name] = partition

    for partition in partitions:
        if partition.type == "app" and partition.subtype == "factory":
            raise VerificationError(
                "factory app partitions are forbidden in the A/B OTA layout"
            )
        if partition.type == "app" and partition.name not in {"ota_0", "ota_1"}:
            raise VerificationError(
                f"unexpected app partition in fixed A/B layout: {partition.name}"
            )

    for name, expected in REQUIRED_PARTITIONS.items():
        partition = by_name.get(name)
        if partition is None:
            raise VerificationError(f"required partition is missing: {name}")
        actual = (
            partition.type,
            partition.subtype,
            partition.offset,
            partition.size,
        )
        if actual != expected:
            raise VerificationError(
                f"partition {name} must be {expected!r}, found {actual!r}"
            )

    ordered = sorted(partitions, key=lambda partition: partition.offset)
    previous: Partition | None = None
    for partition in ordered:
        end = partition.offset + partition.size
        if end > FLASH_SIZE:
            raise VerificationError(
                f"partition {partition.name} ends beyond 8 MiB flash"
            )
        if previous is not None and partition.offset < previous.offset + previous.size:
            raise VerificationError(
                f"partition overlap: {previous.name} and {partition.name}"
            )
        previous = partition


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--partition-table",
        type=Path,
        default=Path("partitions.csv"),
    )
    parser.add_argument("--sdkconfig", type=Path, default=Path("sdkconfig"))
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = make_parser().parse_args(argv)
    try:
        validate_layout(
            parse_partition_csv(args.partition_table.read_text(encoding="utf-8"))
        )
        validate_config(parse_sdkconfig(args.sdkconfig.read_text(encoding="utf-8")))
    except (OSError, VerificationError) as error:
        print(f"OTA layout verification failed: {error}", file=sys.stderr)
        return 1

    print("AirDAP OTA layout verification passed")
    print(f"  partition table: {args.partition_table.resolve()}")
    print(f"  sdkconfig: {args.sdkconfig.resolve()}")
    print("  flash: 8 MiB; ota_0: 4032 KiB; ota_1: 4032 KiB; rollback: enabled")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
