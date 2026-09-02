from __future__ import annotations

import hashlib
import importlib.util
import io
import os
import stat
import subprocess
import tempfile
import unittest
from contextlib import redirect_stderr
from pathlib import Path
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[3] / "tools" / "airdap-sec2-credentials.py"
SPEC = importlib.util.spec_from_file_location("airdap_sec2_credentials", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
credentials = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(credentials)


class Security2CredentialToolTests(unittest.TestCase):
    def test_derivation_matches_espressif_security2_vector(self) -> None:
        salt = bytes.fromhex("036ee0c7bcb9eda84c9eac97d93decf4")
        _, verifier = credentials.derive_salt_and_verifier(
            "abcd1234",
            salt,
            username="wifiprov",
        )
        self.assertEqual(len(verifier), credentials.VERIFIER_SIZE)
        self.assertEqual(
            hashlib.sha256(verifier).hexdigest(),
            "e985f1fb10e7c5aaf625ec7cc0896e8f92bdcd5b621141766ea6ea93bd899b06",
        )

    def test_generator_uses_private_files_and_emits_only_image(self) -> None:
        salt = bytes(range(credentials.SALT_SIZE))
        _, verifier = credentials.derive_salt_and_verifier("test-only-pop", salt)

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            generator = (
                root
                / "idf"
                / "components"
                / "nvs_flash"
                / "nvs_partition_generator"
                / "nvs_partition_gen.py"
            )
            generator.parent.mkdir(parents=True)
            generator.write_text("# fake generator\n", encoding="utf-8")
            output = root / "private" / "sec2_keys.bin"

            def fake_run(
                command: list[str],
                *,
                cwd: Path,
                capture_output: bool,
                text: bool,
                check: bool,
            ) -> subprocess.CompletedProcess[str]:
                self.assertTrue(capture_output and text and not check)
                self.assertNotIn("test-only-pop", " ".join(command))
                csv_text = (cwd / "credentials.csv").read_text(encoding="utf-8")
                self.assertNotIn("test-only-pop", csv_text)
                self.assertEqual((cwd / "salt.bin").read_bytes(), salt)
                self.assertEqual((cwd / "verifier.bin").read_bytes(), verifier)
                self.assertEqual(
                    stat.S_IMODE(Path(command[-2]).stat().st_mode),
                    stat.S_IRUSR | stat.S_IWUSR,
                )
                Path(command[-2]).write_bytes(
                    b"\xff" * credentials.NVS_PARTITION_SIZE
                )
                return subprocess.CompletedProcess(command, 0, "ignored", "")

            fingerprint = credentials.generate_nvs_image(
                output,
                salt,
                verifier,
                root / "idf",
                command_runner=fake_run,
            )
            self.assertEqual(
                fingerprint,
                credentials.credential_fingerprint(salt, verifier),
            )
            self.assertEqual(output.stat().st_size, credentials.NVS_PARTITION_SIZE)
            self.assertEqual(
                stat.S_IMODE(output.stat().st_mode),
                stat.S_IRUSR | stat.S_IWUSR,
            )

    def test_generator_failure_removes_private_output(self) -> None:
        salt = bytes(range(credentials.SALT_SIZE))
        _, verifier = credentials.derive_salt_and_verifier("test-only-pop", salt)

        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            generator = (
                root
                / "idf"
                / "components"
                / "nvs_flash"
                / "nvs_partition_generator"
                / "nvs_partition_gen.py"
            )
            generator.parent.mkdir(parents=True)
            generator.write_text("# fake generator\n", encoding="utf-8")
            output = root / "private" / "sec2_keys.bin"

            def fail_run(*args: object, **kwargs: object) -> subprocess.CompletedProcess[str]:
                return subprocess.CompletedProcess([], 9, "ignored", "ignored")

            with self.assertRaises(credentials.CredentialGenerationError):
                credentials.generate_nvs_image(
                    output,
                    salt,
                    verifier,
                    root / "idf",
                    command_runner=fail_run,
                )
            self.assertFalse(output.exists())

    def test_generator_rejects_repository_output(self) -> None:
        salt = bytes(range(credentials.SALT_SIZE))
        _, verifier = credentials.derive_salt_and_verifier("test-only-pop", salt)
        output = credentials.REPOSITORY_ROOTS[0] / "sec2_keys.bin"

        with self.assertRaisesRegex(
            credentials.CredentialGenerationError,
            "outside the AirDAP repository",
        ):
            credentials.generate_nvs_image(
                output,
                salt,
                verifier,
                Path("/unused"),
            )

    def test_main_rejects_getpass_fallback(self) -> None:
        stderr = io.StringIO()
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "unused.bin"
            with mock.patch.dict(os.environ, {"IDF_PATH": "/unused"}):
                with mock.patch.object(
                    credentials.getpass,
                    "getpass",
                    side_effect=credentials.getpass.GetPassWarning,
                ):
                    with redirect_stderr(stderr):
                        self.assertEqual(credentials.main([str(output)]), 2)
        self.assertIn("requires a private terminal", stderr.getvalue())


if __name__ == "__main__":
    unittest.main()
