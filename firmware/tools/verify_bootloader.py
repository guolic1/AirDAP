#!/usr/bin/env python3
"""Verify the AirDAP second-stage bootloader's final artifact contract."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path
from typing import Mapping, Sequence


EXPECTED_WDT_TIME_MS = "9000"
REQUIRED_STRONG_SYMBOLS = (
    "bootloader_before_init",
    "bootloader_hooks_include",
)
FORBIDDEN_VALIDATION_BYPASSES = (
    "CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP",
    "CONFIG_BOOTLOADER_SKIP_VALIDATE_ON_POWER_ON",
    "CONFIG_BOOTLOADER_SKIP_VALIDATE_ALWAYS",
)

_UNSET_CONFIG = re.compile(r"^# (CONFIG_[A-Z0-9_]+) is not set$")
_FUNCTION_HEADER = re.compile(r"^[0-9a-fA-F]+ <([^>]+)>:\s*$", re.MULTILINE)


class VerificationError(RuntimeError):
    """Raised when the built bootloader violates the AirDAP contract."""


def parse_sdkconfig(text: str) -> dict[str, str | None]:
    """Parse assignments and explicit disabled values from an sdkconfig file."""
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
    """Validate boot-time protection and image-validation configuration."""
    target = config.get("CONFIG_IDF_TARGET")
    if target is None or target.strip('"') != "esp32s3":
        raise VerificationError(
            f"bootloader must target ESP32-S3, found {target!r}"
        )

    _require_config(
        config,
        "CONFIG_BOOTLOADER_REGION_PROTECTION_ENABLE",
        "y",
        "bootloader region protection",
    )
    _require_config(
        config,
        "CONFIG_BOOTLOADER_WDT_ENABLE",
        "y",
        "bootloader watchdog",
    )
    _require_config(
        config,
        "CONFIG_BOOTLOADER_WDT_TIME_MS",
        EXPECTED_WDT_TIME_MS,
        "bootloader watchdog timeout",
    )

    for option in FORBIDDEN_VALIDATION_BYPASSES:
        if config.get(option) == "y":
            raise VerificationError(
                f"application image validation bypass must remain disabled: {option}"
            )


def validate_symbols(nm_output: str) -> None:
    """Require both ESP-IDF hook symbols to be strongly defined in executable text."""
    symbols: dict[str, str] = {}
    for line in nm_output.splitlines():
        fields = line.split()
        if len(fields) >= 2:
            symbols[fields[-1]] = fields[-2]

    for name in REQUIRED_STRONG_SYMBOLS:
        symbol_type = symbols.get(name)
        if symbol_type != "T":
            raise VerificationError(
                f"{name} must be a strong text symbol (T), found {symbol_type!r}"
            )


def _entrypoint_block(disassembly: str) -> str:
    headers = list(_FUNCTION_HEADER.finditer(disassembly))
    for index, header in enumerate(headers):
        if header.group(1) != "call_start_cpu0":
            continue
        end = headers[index + 1].start() if index + 1 < len(headers) else len(disassembly)
        return disassembly[header.start():end]
    raise VerificationError("call_start_cpu0 disassembly is missing")


def _find_direct_call(block: str, symbol: str) -> re.Match[str]:
    call = re.search(
        rf"\bcall[0-9]*\b[^\n]*<{re.escape(symbol)}>",
        block,
    )
    if call is None:
        raise VerificationError(
            f"call_start_cpu0 does not directly call {symbol}"
        )
    return call


def validate_call_order(disassembly: str) -> None:
    """Prove the linked entrypoint calls the AirDAP hook before IDF init."""
    block = _entrypoint_block(disassembly)
    hook_call = _find_direct_call(block, "bootloader_before_init")
    init_call = _find_direct_call(block, "bootloader_init")
    if hook_call.start() >= init_call.start():
        raise VerificationError(
            "bootloader_before_init must be called before bootloader_init"
        )


def _run_tool(command: Sequence[str]) -> str:
    try:
        completed = subprocess.run(
            command,
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
    except FileNotFoundError as error:
        raise VerificationError(f"tool not found: {command[0]}") from error
    except subprocess.CalledProcessError as error:
        detail = error.stderr.strip() or error.stdout.strip() or f"exit {error.returncode}"
        raise VerificationError(f"{' '.join(command)} failed: {detail}") from error
    return completed.stdout


def verify_artifacts(
    elf: Path,
    sdkconfig: Path,
    nm: str,
    objdump: str,
) -> None:
    if not elf.is_file():
        raise VerificationError(f"bootloader ELF does not exist: {elf}")
    if not sdkconfig.is_file():
        raise VerificationError(f"sdkconfig does not exist: {sdkconfig}")

    validate_config(parse_sdkconfig(sdkconfig.read_text(encoding="utf-8")))
    validate_symbols(_run_tool((nm, "--defined-only", str(elf))))
    validate_call_order(
        _run_tool((objdump, "-d", "--disassemble=call_start_cpu0", str(elf)))
    )


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--build-dir",
        type=Path,
        default=Path("build"),
        help="ESP-IDF build directory (default: build)",
    )
    parser.add_argument("--elf", type=Path, help="override bootloader ELF path")
    parser.add_argument("--sdkconfig", type=Path, help="override generated sdkconfig path")
    parser.add_argument("--nm", default="xtensa-esp32s3-elf-nm")
    parser.add_argument("--objdump", default="xtensa-esp32s3-elf-objdump")
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = make_parser().parse_args(argv)
    elf = args.elf or args.build_dir / "bootloader" / "bootloader.elf"
    sdkconfig = args.sdkconfig or args.build_dir.parent / "sdkconfig"
    try:
        verify_artifacts(elf, sdkconfig, args.nm, args.objdump)
    except (OSError, VerificationError) as error:
        print(f"bootloader artifact verification failed: {error}", file=sys.stderr)
        return 1

    print("AirDAP bootloader artifact verification passed")
    print(f"  ELF: {elf.resolve()}")
    print(f"  sdkconfig: {sdkconfig.resolve()}")
    print("  hook: strong and called before bootloader_init")
    print("  protections: region protection, RTC WDT, full image validation")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
