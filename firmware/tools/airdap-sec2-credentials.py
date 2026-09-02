#!/usr/bin/env python3
"""Generate a private Security 2 NVS image without persisting the PoP."""

from __future__ import annotations

import argparse
import getpass
import hashlib
import os
import secrets
import subprocess
import sys
import tempfile
import warnings
from collections.abc import Callable, Sequence
from pathlib import Path


SECURITY2_USERNAME = "airdap"
SALT_SIZE = 16
VERIFIER_SIZE = 384
NVS_PARTITION_SIZE = 0x3000
FINGERPRINT_DOMAIN = b"AirDAP-Security2-v1"
SRP_GENERATOR = 5
SRP_3072_MODULUS = int(
    "FFFFFFFFFFFFFFFFC90FDAA22168C234C4C6628B80DC1CD129024E088A67CC"
    "74020BBEA63B139B22514A08798E3404DDEF9519B3CD3A431B302B0A6DF25F"
    "14374FE1356D6D51C245E485B576625E7EC6F44C42E9A637ED6B0BFF5CB6F4"
    "06B7EDEE386BFB5A899FA5AE9F24117C4B1FE649286651ECE45B3DC2007CB8"
    "A163BF0598DA48361C55D39A69163FA8FD24CF5F83655D23DCA3AD961C62F35"
    "6208552BB9ED529077096966D670C354E4ABC9804F1746C08CA18217C32905E"
    "462E36CE3BE39E772C180E86039B2783A2EC07A28FB5C55DF06F4C52C9DE2B"
    "CBF6955817183995497CEA956AE515D2261898FA051015728E5A8AAAC42DAD3"
    "3170D04507A33A85521ABDF1CBA64ECFB850458DBEF0A8AEA71575D060C7DB3"
    "970F85A6E1E4C7ABF5AE8CDB0933D71E8C94E04A25619DCEE3D2261AD2EE6"
    "BF12FFA06D98A0864D87602733EC86A64521F2B18177B200CBBE117577A615D"
    "6C770988C0BAD946E208E24FA074E5AB3143DB5BFCE0FD108E4B82D120A93AD"
    "2CAFFFFFFFFFFFFFFFF",
    16,
)

CommandRunner = Callable[..., subprocess.CompletedProcess[str]]


class CredentialGenerationError(RuntimeError):
    """Raised when a private credential image cannot be produced safely."""


def _repository_roots() -> tuple[Path, ...]:
    worktree_root = Path(__file__).resolve().parents[2]
    roots = {worktree_root}
    dot_git = worktree_root / ".git"
    if dot_git.is_file():
        try:
            marker = dot_git.read_text(encoding="utf-8").strip()
            if marker.startswith("gitdir:"):
                git_dir = Path(marker.removeprefix("gitdir:").strip())
                if not git_dir.is_absolute():
                    git_dir = (worktree_root / git_dir).resolve()
                common_marker = git_dir / "commondir"
                if common_marker.is_file():
                    common_dir = Path(
                        common_marker.read_text(encoding="utf-8").strip()
                    )
                    if not common_dir.is_absolute():
                        common_dir = (git_dir / common_dir).resolve()
                    if common_dir.name == ".git":
                        roots.add(common_dir.parent)
        except OSError as error:
            raise CredentialGenerationError(
                "cannot resolve the AirDAP common repository boundary"
            ) from error
    return tuple(roots)


REPOSITORY_ROOTS = _repository_roots()


def _require_external_output(output: Path) -> Path:
    resolved = output.expanduser().resolve()
    for repository_root in REPOSITORY_ROOTS:
        try:
            resolved.relative_to(repository_root)
        except ValueError:
            continue
        raise CredentialGenerationError(
            "output must be in a private directory outside the AirDAP repository"
        )
    return resolved


def derive_salt_and_verifier(
    password: str,
    salt: bytes,
    *,
    username: str = SECURITY2_USERNAME,
) -> tuple[bytes, bytes]:
    """Derive Espressif Security 2's SHA-512/SRP-3072 verifier."""
    if not 8 <= len(password) <= 64:
        raise ValueError("Security 2 PoP must contain 8..64 characters")
    if len(salt) != SALT_SIZE:
        raise ValueError(f"Security 2 salt must contain {SALT_SIZE} bytes")

    identity_hash = hashlib.sha512(
        f"{username}:{password}".encode("utf-8")
    ).digest()
    private_key = int.from_bytes(
        hashlib.sha512(salt + identity_hash).digest(),
        "big",
    )
    verifier = pow(SRP_GENERATOR, private_key, SRP_3072_MODULUS).to_bytes(
        VERIFIER_SIZE,
        "big",
    )
    return salt, verifier


def credential_fingerprint(salt: bytes, verifier: bytes) -> str:
    if len(salt) != SALT_SIZE or len(verifier) != VERIFIER_SIZE:
        raise ValueError("invalid Security 2 credential lengths")
    return hashlib.sha256(FINGERPRINT_DOMAIN + salt + verifier).hexdigest().upper()


def _nvs_generator(idf_path: Path) -> Path:
    generator = (
        idf_path
        / "components"
        / "nvs_flash"
        / "nvs_partition_generator"
        / "nvs_partition_gen.py"
    )
    if not generator.is_file():
        raise CredentialGenerationError(
            "ESP-IDF NVS partition generator was not found under IDF_PATH"
        )
    return generator


def generate_nvs_image(
    output: Path,
    salt: bytes,
    verifier: bytes,
    idf_path: Path,
    *,
    force: bool = False,
    command_runner: CommandRunner = subprocess.run,
) -> str:
    """Create an NVS image using ESP-IDF's checked installation tool."""
    fingerprint = credential_fingerprint(salt, verifier)
    output = _require_external_output(output)
    if output.exists() and not force:
        raise CredentialGenerationError(
            "output already exists; pass --force to replace the selected file"
        )
    output.parent.mkdir(parents=True, exist_ok=True)
    generator = _nvs_generator(idf_path.resolve())

    with tempfile.TemporaryDirectory(
        prefix=".airdap-sec2-",
        dir=output.parent,
    ) as temporary:
        private_dir = Path(temporary)
        os.chmod(private_dir, 0o700)
        salt_path = private_dir / "salt.bin"
        verifier_path = private_dir / "verifier.bin"
        csv_path = private_dir / "credentials.csv"
        image_path = private_dir / "sec2_keys.bin"
        salt_path.write_bytes(salt)
        verifier_path.write_bytes(verifier)
        csv_path.write_text(
            "key,type,encoding,value\n"
            "security2,namespace,,\n"
            "salt,file,binary,salt.bin\n"
            "verifier,file,binary,verifier.bin\n",
            encoding="utf-8",
        )
        for private_file in (salt_path, verifier_path, csv_path):
            os.chmod(private_file, 0o600)
        image_fd = os.open(
            image_path,
            os.O_WRONLY | os.O_CREAT | os.O_EXCL,
            0o600,
        )
        os.close(image_fd)

        result = command_runner(
            [
                sys.executable,
                str(generator),
                "generate",
                csv_path.name,
                str(image_path),
                hex(NVS_PARTITION_SIZE),
            ],
            cwd=private_dir,
            capture_output=True,
            text=True,
            check=False,
        )
        if result.returncode != 0:
            raise CredentialGenerationError(
                f"ESP-IDF NVS generator failed with exit code {result.returncode}"
            )
        if not image_path.is_file() or image_path.stat().st_size != NVS_PARTITION_SIZE:
            raise CredentialGenerationError(
                "ESP-IDF NVS generator produced an unexpected image size"
            )
        os.chmod(image_path, 0o600)
        os.replace(image_path, output)
        os.chmod(output, 0o600)
    return fingerprint


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "output",
        type=Path,
        help="private, repository-external 0x3000-byte sec2_keys image path",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="replace the explicitly selected output file",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = make_parser().parse_args(argv)
    idf_path_value = os.environ.get("IDF_PATH")
    if not idf_path_value:
        print("IDF_PATH is not set; activate ESP-IDF first", file=sys.stderr)
        return 2

    try:
        output = _require_external_output(args.output)
    except CredentialGenerationError as error:
        print(f"Security 2 credential generation failed: {error}", file=sys.stderr)
        return 1

    try:
        with warnings.catch_warnings():
            warnings.simplefilter("error", getpass.GetPassWarning)
            password = getpass.getpass("Security 2 test PoP: ")
            confirmation = getpass.getpass("Confirm Security 2 test PoP: ")
    except getpass.GetPassWarning:
        print(
            "Security 2 credential generation requires a private terminal",
            file=sys.stderr,
        )
        return 2
    if password != confirmation:
        print("Security 2 PoP confirmation does not match", file=sys.stderr)
        return 2
    try:
        salt, verifier = derive_salt_and_verifier(
            password,
            secrets.token_bytes(SALT_SIZE),
        )
        fingerprint = generate_nvs_image(
            output,
            salt,
            verifier,
            Path(idf_path_value),
            force=args.force,
        )
    except (OSError, ValueError, CredentialGenerationError) as error:
        print(f"Security 2 credential generation failed: {error}", file=sys.stderr)
        return 1
    finally:
        password = ""
        confirmation = ""

    print(f"SEC2_CREDENTIAL_FINGERPRINT={fingerprint}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
