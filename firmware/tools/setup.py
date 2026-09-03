#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from collections.abc import Callable, Sequence
from pathlib import Path


FIRMWARE_DIR = Path(__file__).resolve().parents[1]
ENVIRONMENT_DIR = FIRMWARE_DIR / ".airdap-env"
DEFAULT_IDF_PATH = ENVIRONMENT_DIR / "esp-idf"
CONFIGURED_IDF_PATH_FILE = ENVIRONMENT_DIR / "idf-path.txt"

DEFAULT_IDF_REPOSITORY = "https://github.com/espressif/esp-idf.git"
DEFAULT_IDF_TAG = "v6.1"
DEFAULT_IDF_COMMIT = "fff9895c82d744c7237be8847347bdd1b07c6643"
REQUIRED_IDF_VERSION = (6, 1, 0)
MANAGED_IDF_MODE = "managed"
EXTERNAL_IDF_MODE = "external"
REQUIRED_IDF_SUBMODULES = (
    "components/bootloader/subproject/components/micro-ecc/micro-ecc",
    "components/bt/controller/lib_esp32c3_family",
    "components/bt/host/nimble/nimble",
    "components/esp_coex/lib",
    "components/esp_phy/lib",
    "components/esp_wifi/lib",
    "components/heap/tlsf",
    "components/lwip/lwip",
    "components/mbedtls/mbedtls",
    "components/protobuf-c/protobuf-c",
)
REQUIRED_IDF_TOOLS = (
    "xtensa-esp-elf",
    "cmake",
    "ninja",
    "esp-rom-elfs",
)

_VERSION_PATTERN = re.compile(
    r"^set\(IDF_VERSION_(MAJOR|MINOR|PATCH)\s+([0-9]+)\)\s*$",
    re.MULTILINE,
)


class SetupError(RuntimeError):
    pass


RunCommand = Callable[..., str | None]
InstallManaged = Callable[[Path, Path], None]


def run_command(
    command: list[str],
    *,
    cwd: Path | None = None,
    env: dict[str, str] | None = None,
    capture_output: bool = False,
) -> str | None:
    try:
        result = subprocess.run(
            command,
            cwd=cwd,
            env=env,
            check=True,
            text=True,
            stdout=subprocess.PIPE if capture_output else None,
        )
    except FileNotFoundError as error:
        raise SetupError(f"required command is missing: {command[0]}") from error
    except subprocess.CalledProcessError as error:
        raise SetupError(
            f"command failed with exit code {error.returncode}: {' '.join(command)}"
        ) from error
    except OSError as error:
        raise SetupError(f"cannot run required command {command[0]}: {error}") from error
    return result.stdout.strip() if capture_output and result.stdout is not None else None


def read_idf_version(idf_path: Path) -> tuple[int, int, int]:
    version_file = idf_path / "tools" / "cmake" / "version.cmake"
    try:
        contents = version_file.read_text(encoding="utf-8")
    except OSError as error:
        raise SetupError(f"cannot read ESP-IDF version file {version_file}: {error}") from error

    values = {name: int(value) for name, value in _VERSION_PATTERN.findall(contents)}
    missing = [name for name in ("MAJOR", "MINOR", "PATCH") if name not in values]
    if missing:
        raise SetupError(
            f"ESP-IDF version file {version_file} is missing: {', '.join(missing)}"
        )
    return values["MAJOR"], values["MINOR"], values["PATCH"]


def validate_idf_path(candidate: Path) -> Path:
    idf_path = candidate.expanduser().resolve()
    required_files = (
        "tools/idf.py",
        "tools/idf_tools.py",
        "tools/activate.py",
        "tools/cmake/project.cmake",
        "tools/cmake/version.cmake",
        "export.sh",
        "export.ps1",
    )
    missing = [relative for relative in required_files if not (idf_path / relative).is_file()]
    if missing:
        raise SetupError(
            f"ESP-IDF path {idf_path} is missing required files: {', '.join(missing)}"
        )

    version = read_idf_version(idf_path)
    if version != REQUIRED_IDF_VERSION:
        actual = ".".join(str(part) for part in version)
        expected = ".".join(str(part) for part in REQUIRED_IDF_VERSION)
        raise SetupError(
            f"ESP-IDF path {idf_path} has version {actual}; AirDAP requires {expected}"
        )
    return idf_path


def clone_pinned_idf(
    idf_path: Path,
    *,
    run_command: RunCommand = run_command,
) -> None:
    try:
        idf_path.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="esp-idf-", dir=idf_path.parent) as temporary:
            checkout = Path(temporary) / "esp-idf"
            run_command(
                [
                    "git",
                    "clone",
                    "--branch",
                    DEFAULT_IDF_TAG,
                    "--depth",
                    "1",
                    DEFAULT_IDF_REPOSITORY,
                    str(checkout),
                ]
            )
            revision = run_command(
                ["git", "-C", str(checkout), "rev-parse", "HEAD"],
                capture_output=True,
            )
            if revision != DEFAULT_IDF_COMMIT:
                raise SetupError(
                    f"downloaded ESP-IDF {DEFAULT_IDF_TAG} resolved to {revision}; "
                    f"expected {DEFAULT_IDF_COMMIT}"
                )
            validate_idf_path(checkout)
            checkout.replace(idf_path)
    except OSError as error:
        raise SetupError(f"cannot install managed ESP-IDF at {idf_path}: {error}") from error


def verify_managed_checkout(
    idf_path: Path,
    *,
    run_command: RunCommand = run_command,
) -> None:
    validate_idf_path(idf_path)
    revision = run_command(
        ["git", "-C", str(idf_path), "rev-parse", "HEAD"],
        capture_output=True,
    )
    if revision != DEFAULT_IDF_COMMIT:
        raise SetupError(
            f"managed ESP-IDF at {idf_path} is revision {revision}; "
            f"expected {DEFAULT_IDF_COMMIT}"
        )


def install_idf_tools(
    idf_path: Path,
    tools_path: Path,
    *,
    run_command: RunCommand = run_command,
) -> None:
    idf_path = idf_path.resolve()
    tools_path = tools_path.resolve()
    environment = os.environ.copy()
    environment["IDF_PATH"] = str(idf_path)
    environment["IDF_TOOLS_PATH"] = str(tools_path)
    environment.pop("IDF_PYTHON_ENV_PATH", None)
    idf_tools = str(idf_path / "tools" / "idf_tools.py")

    run_command(
        [
            sys.executable,
            idf_tools,
            "--idf-path",
            str(idf_path),
            "install",
            "--targets=esp32s3",
            *REQUIRED_IDF_TOOLS,
        ],
        env=environment,
    )
    run_command(
        [
            sys.executable,
            idf_tools,
            "--idf-path",
            str(idf_path),
            "install-python-env",
            "--features=core",
        ],
        env=environment,
    )


def install_external_environment(idf_path: Path, tools_path: Path) -> None:
    configured = validate_idf_path(idf_path)
    install_idf_tools(configured, tools_path)


def install_managed_environment(
    idf_path: Path,
    tools_path: Path,
    *,
    run_command: RunCommand = run_command,
) -> None:
    idf_path = idf_path.resolve()
    if idf_path.exists():
        verify_managed_checkout(idf_path, run_command=run_command)
    else:
        clone_pinned_idf(idf_path, run_command=run_command)
    # Keep the verified main checkout when a submodule download fails so the
    # next setup run can resume instead of cloning ESP-IDF again.
    run_command(
        [
            "git",
            "-C",
            str(idf_path),
            "submodule",
            "update",
            "--init",
            "--recursive",
            "--depth",
            "1",
            "--",
            *REQUIRED_IDF_SUBMODULES,
        ]
    )
    install_idf_tools(idf_path, tools_path, run_command=run_command)


def save_idf_configuration(idf_path: Path, idf_mode: str, config_file: Path) -> None:
    if idf_mode not in (MANAGED_IDF_MODE, EXTERNAL_IDF_MODE):
        raise SetupError(f"invalid ESP-IDF configuration mode: {idf_mode}")
    config_file.parent.mkdir(parents=True, exist_ok=True)
    temporary_name: str | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            dir=config_file.parent,
            prefix=f".{config_file.name}.",
            delete=False,
        ) as temporary:
            temporary.write(f"{idf_path.resolve()}\n{idf_mode}\n")
            temporary_name = temporary.name
        os.replace(temporary_name, config_file)
        temporary_name = None
    except OSError as error:
        raise SetupError(f"cannot save ESP-IDF path to {config_file}: {error}") from error
    finally:
        if temporary_name is not None:
            try:
                Path(temporary_name).unlink()
            except FileNotFoundError:
                pass


def configure_environment(
    requested_idf_path: Path | None,
    *,
    config_file: Path = CONFIGURED_IDF_PATH_FILE,
    default_idf_path: Path = DEFAULT_IDF_PATH,
    tools_path: Path = ENVIRONMENT_DIR,
    install_managed: InstallManaged = install_managed_environment,
    install_external: InstallManaged = install_external_environment,
) -> Path:
    if requested_idf_path is None:
        install_managed(default_idf_path, tools_path)
        configured = validate_idf_path(default_idf_path)
        idf_mode = MANAGED_IDF_MODE
    else:
        configured = validate_idf_path(requested_idf_path)
        install_external(configured, tools_path)
        idf_mode = EXTERNAL_IDF_MODE
    save_idf_configuration(configured, idf_mode, config_file)
    return configured


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Configure AirDAP's ESP-IDF environment. Without a path, download "
            "and install the repository-managed ESP-IDF v6.1 environment."
        )
    )
    parser.add_argument(
        "idf_path",
        nargs="?",
        type=Path,
        help="existing ESP-IDF 6.1.0 source directory",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    arguments = make_parser().parse_args(argv)
    try:
        configured = configure_environment(arguments.idf_path)
    except SetupError as error:
        print(f"setup.py: {error}", file=sys.stderr)
        return 1
    print(f"AirDAP ESP-IDF environment configured: {configured}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
