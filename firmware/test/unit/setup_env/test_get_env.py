from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


FIRMWARE_DIR = Path(__file__).resolve().parents[3]
GET_ENV_SH = FIRMWARE_DIR / "get_env.sh"
GET_ENV_PS1 = FIRMWARE_DIR / "get_env.ps1"


class GetEnvironmentTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.firmware = Path(self.temporary_directory.name) / "firmware"
        self.firmware.mkdir()

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def make_idf(self, path: Path) -> Path:
        path.mkdir(parents=True)
        (path / "export.sh").write_text(
            'export IDF_PATH="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"\n'
            'export AIRDAP_TEST_ACTIVATED="$IDF_PATH"\n',
            encoding="utf-8",
        )
        (path / "export.ps1").write_text(
            "$env:IDF_PATH = $PSScriptRoot\n"
            "$env:AIRDAP_TEST_ACTIVATED = $env:IDF_PATH\n",
            encoding="utf-8",
        )
        return path

    def configure(self, idf_path: str) -> None:
        state = self.firmware / ".airdap-env"
        state.mkdir(exist_ok=True)
        (state / "idf-path.txt").write_text(idf_path + "\n", encoding="utf-8")

    def require_bash(self) -> None:
        if os.name == "nt" or shutil.which("bash") is None:
            self.skipTest("Bash activation is tested only on Linux hosts")

    def test_bash_sources_saved_external_idf_path(self) -> None:
        self.require_bash()
        shutil.copy2(GET_ENV_SH, self.firmware / "get_env.sh")
        idf_path = self.make_idf(Path(self.temporary_directory.name) / "external idf")
        self.configure(str(idf_path.resolve()))

        result = subprocess.run(
            [
                "bash",
                "-c",
                '. "$1" && printf "%s\\n" "$AIRDAP_TEST_ACTIVATED"',
                "bash",
                str(self.firmware / "get_env.sh"),
            ],
            check=False,
            text=True,
            capture_output=True,
            env={key: value for key, value in os.environ.items() if key != "IDF_TOOLS_PATH"},
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip().splitlines()[-1], str(idf_path.resolve()))

    def test_bash_uses_repository_tools_for_external_idf(self) -> None:
        self.require_bash()
        shutil.copy2(GET_ENV_SH, self.firmware / "get_env.sh")
        state = self.firmware / ".airdap-env"
        idf_path = self.make_idf(Path(self.temporary_directory.name) / "external idf")
        self.configure(str(idf_path.resolve()))

        result = subprocess.run(
            [
                "bash",
                "-c",
                '. "$1" && printf "%s|%s\\n" "$IDF_TOOLS_PATH" "${IDF_PYTHON_ENV_PATH-unset}"',
                "bash",
                str(self.firmware / "get_env.sh"),
            ],
            check=False,
            text=True,
            capture_output=True,
            env={**os.environ, "IDF_PYTHON_ENV_PATH": "/stale/python"},
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            result.stdout.strip().splitlines()[-1],
            f"{state.resolve()}|unset",
        )

    def test_bash_sets_repository_tools_path_for_managed_idf(self) -> None:
        self.require_bash()
        shutil.copy2(GET_ENV_SH, self.firmware / "get_env.sh")
        state = self.firmware / ".airdap-env"
        idf_path = self.make_idf(state / "esp-idf")
        self.configure(str(idf_path.resolve()))

        result = subprocess.run(
            [
                "bash",
                "-c",
                '. "$1" && printf "%s\\n" "$IDF_TOOLS_PATH"',
                "bash",
                str(self.firmware / "get_env.sh"),
            ],
            check=False,
            text=True,
            capture_output=True,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip().splitlines()[-1], str(state.resolve()))

    def test_bash_rejects_missing_configuration(self) -> None:
        self.require_bash()
        shutil.copy2(GET_ENV_SH, self.firmware / "get_env.sh")

        result = subprocess.run(
            ["bash", "-c", '. "$1"', "bash", str(self.firmware / "get_env.sh")],
            check=False,
            text=True,
            capture_output=True,
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("tools/setup.py", result.stderr)

    def test_bash_requires_sourcing(self) -> None:
        self.require_bash()
        shutil.copy2(GET_ENV_SH, self.firmware / "get_env.sh")

        result = subprocess.run(
            ["bash", str(self.firmware / "get_env.sh")],
            check=False,
            text=True,
            capture_output=True,
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("source", result.stderr)

    def test_powershell_sources_saved_external_idf_path_when_available(self) -> None:
        powershell = shutil.which("pwsh") or shutil.which("pwsh.exe")
        if powershell is None:
            self.skipTest("PowerShell is unavailable on this host")

        shutil.copy2(GET_ENV_PS1, self.firmware / "get_env.ps1")
        idf_path = self.make_idf(Path(self.temporary_directory.name) / "external idf")
        script_path = self._powershell_path(self.firmware / "get_env.ps1", powershell)
        configured_path = self._powershell_path(idf_path, powershell)
        self.configure(configured_path)
        escaped_script = script_path.replace("'", "''")

        result = subprocess.run(
            [
                powershell,
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-Command",
                f". '{escaped_script}'; Write-Output $env:AIRDAP_TEST_ACTIVATED",
            ],
            check=False,
            text=True,
            capture_output=True,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(
            result.stdout.strip(),
            f"PowerShell produced no activation output; stderr={result.stderr!r}",
        )
        self.assertEqual(
            result.stdout.strip().splitlines()[-1].casefold(),
            configured_path.casefold(),
        )

    def test_powershell_sets_repository_tools_path_for_managed_idf_when_available(
        self,
    ) -> None:
        powershell = shutil.which("pwsh") or shutil.which("pwsh.exe")
        if powershell is None:
            self.skipTest("PowerShell is unavailable on this host")

        shutil.copy2(GET_ENV_PS1, self.firmware / "get_env.ps1")
        state = self.firmware / ".airdap-env"
        idf_path = self.make_idf(state / "esp-idf")
        script_path = self._powershell_path(self.firmware / "get_env.ps1", powershell)
        configured_path = self._powershell_path(idf_path, powershell)
        expected_tools_path = self._powershell_path(state, powershell)
        self.configure(configured_path)
        escaped_script = script_path.replace("'", "''")

        result = subprocess.run(
            [
                powershell,
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-Command",
                f". '{escaped_script}'; Write-Output $env:IDF_TOOLS_PATH",
            ],
            check=False,
            text=True,
            capture_output=True,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            result.stdout.strip().splitlines()[-1].casefold(),
            expected_tools_path.casefold(),
        )

    @staticmethod
    def _powershell_path(path: Path, executable: str) -> str:
        if os.name == "nt" or not executable.lower().endswith(".exe"):
            return str(path.resolve())
        return subprocess.run(
            ["wslpath", "-w", str(path.resolve())],
            check=True,
            text=True,
            capture_output=True,
        ).stdout.strip()


if __name__ == "__main__":
    unittest.main()
