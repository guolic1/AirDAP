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
        tools = path / "tools"
        tools.mkdir()
        (tools / "activate.py").write_text(
            "from pathlib import Path\n"
            "print(Path(__file__).resolve().parents[1] / 'export.ps1')\n",
            encoding="utf-8",
        )
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

    def configure(self, idf_path: str, mode: str | None = None) -> None:
        state = self.firmware / ".airdap-env"
        state.mkdir(exist_ok=True)
        contents = idf_path + "\n"
        if mode is not None:
            contents += mode + "\n"
        (state / "idf-path.txt").write_text(contents, encoding="utf-8")

    def require_bash(self) -> None:
        if os.name == "nt" or shutil.which("bash") is None:
            self.skipTest("Bash activation is tested only on Linux hosts")

    def test_bash_sources_saved_external_idf_path(self) -> None:
        self.require_bash()
        shutil.copy2(GET_ENV_SH, self.firmware / "get_env.sh")
        idf_path = self.make_idf(Path(self.temporary_directory.name) / "external idf")
        self.configure(str(idf_path.resolve()), "external")

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
        self.configure(str(idf_path.resolve()), "external")

        result = subprocess.run(
            [
                "bash",
                "-c",
                '. "$1" && printf "%s|%s|%s|%s\\n" "$IDF_TOOLS_PATH" '
                '"${IDF_PYTHON_ENV_PATH-unset}" "$IDF_SKIP_TOOLS_CHECK" '
                '"${IDF_SKIP_CHECK_SUBMODULES-unset}"',
                "bash",
                str(self.firmware / "get_env.sh"),
            ],
            check=False,
            text=True,
            capture_output=True,
            env={
                **os.environ,
                "IDF_PYTHON_ENV_PATH": "/stale/python",
                "IDF_SKIP_TOOLS_CHECK": "stale",
                "IDF_SKIP_CHECK_SUBMODULES": "stale",
            },
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            result.stdout.strip().splitlines()[-1],
            f"{state.resolve()}|unset|1|unset",
        )

    def test_bash_sets_repository_tools_path_for_managed_idf(self) -> None:
        self.require_bash()
        shutil.copy2(GET_ENV_SH, self.firmware / "get_env.sh")
        state = self.firmware / ".airdap-env"
        idf_path = self.make_idf(state / "esp-idf")
        self.configure(str(idf_path.resolve()), "managed")

        result = subprocess.run(
            [
                "bash",
                "-c",
                '. "$1" && printf "%s|%s|%s\\n" "$IDF_TOOLS_PATH" '
                '"$IDF_SKIP_TOOLS_CHECK" "$IDF_SKIP_CHECK_SUBMODULES"',
                "bash",
                str(self.firmware / "get_env.sh"),
            ],
            check=False,
            text=True,
            capture_output=True,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(
            result.stdout.strip().splitlines()[-1],
            f"{state.resolve()}|1|1",
        )

    def test_bash_external_mode_overrides_managed_path_location(self) -> None:
        self.require_bash()
        shutil.copy2(GET_ENV_SH, self.firmware / "get_env.sh")
        state = self.firmware / ".airdap-env"
        idf_path = self.make_idf(state / "esp-idf")
        self.configure(str(idf_path.resolve()), "external")

        result = subprocess.run(
            [
                "bash",
                "-c",
                '. "$1" && printf "%s\\n" "${IDF_SKIP_CHECK_SUBMODULES-unset}"',
                "bash",
                str(self.firmware / "get_env.sh"),
            ],
            check=False,
            text=True,
            capture_output=True,
            env={**os.environ, "IDF_SKIP_CHECK_SUBMODULES": "stale"},
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip().splitlines()[-1], "unset")

    def test_bash_accepts_legacy_one_line_configuration(self) -> None:
        self.require_bash()
        shutil.copy2(GET_ENV_SH, self.firmware / "get_env.sh")
        idf_path = self.make_idf(Path(self.temporary_directory.name) / "legacy idf")
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
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip().splitlines()[-1], str(idf_path.resolve()))

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
        state = self.firmware / ".airdap-env"
        idf_path = self.make_idf(state / "esp-idf")
        script_path = self._powershell_path(self.firmware / "get_env.ps1", powershell)
        configured_path = self._powershell_path(idf_path, powershell)
        self.configure(configured_path, "external")
        escaped_script = script_path.replace("'", "''")

        result = subprocess.run(
            [
                powershell,
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-Command",
                "$env:IDF_SKIP_TOOLS_CHECK = 'stale'; "
                "$env:IDF_SKIP_CHECK_SUBMODULES = 'stale'; "
                f". '{escaped_script}'; Write-Output $env:AIRDAP_TEST_ACTIVATED; "
                "Write-Output $env:IDF_SKIP_TOOLS_CHECK; "
                "if (Test-Path Env:IDF_SKIP_CHECK_SUBMODULES) { "
                "Write-Output $env:IDF_SKIP_CHECK_SUBMODULES } else { "
                "Write-Output '<unset>' }",
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
        output_lines = result.stdout.strip().splitlines()
        self.assertEqual(output_lines[-3].casefold(), configured_path.casefold())
        self.assertEqual(output_lines[-2], "1")
        self.assertEqual(output_lines[-1], "<unset>")

    def test_powershell_prefers_uv_launcher_when_available(self) -> None:
        powershell = shutil.which("pwsh") or shutil.which("pwsh.exe")
        if powershell is None:
            self.skipTest("PowerShell is unavailable on this host")

        shutil.copy2(GET_ENV_PS1, self.firmware / "get_env.ps1")
        idf_path = self.make_idf(Path(self.temporary_directory.name) / "external idf")
        script_path = self._powershell_path(self.firmware / "get_env.ps1", powershell)
        configured_path = self._powershell_path(idf_path, powershell)
        project_path = self._powershell_path(self.firmware.parent, powershell)
        activate_path = self._powershell_path(
            idf_path / "tools" / "activate.py", powershell
        )
        export_path = self._powershell_path(idf_path / "export.ps1", powershell)
        self.configure(configured_path, "external")
        escaped_script = script_path.replace("'", "''")
        escaped_export = export_path.replace("'", "''")

        result = subprocess.run(
            [
                powershell,
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-Command",
                "function global:uv { "
                "$env:AIRDAP_TEST_UV_ARGS = $args -join '|'; "
                "$global:LASTEXITCODE = 0; "
                f"Write-Output '{escaped_export}' }}; "
                "function global:python { "
                "$env:AIRDAP_TEST_PYTHON_CALLED = 'yes'; "
                "throw 'python fallback must not run' }; "
                f". '{escaped_script}'; "
                "Write-Output \"uv=[$env:AIRDAP_TEST_UV_ARGS]\"; "
                "Write-Output \"python=[$env:AIRDAP_TEST_PYTHON_CALLED]\"",
            ],
            check=False,
            text=True,
            capture_output=True,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        expected_args = (
            f"run|--project|{project_path}|--locked|python|"
            f"{activate_path}|--export|--shell|powershell"
        )
        self.assertIn(f"uv=[{expected_args}]", result.stdout)
        self.assertIn("python=[]", result.stdout)

    def test_powershell_falls_back_to_python_when_uv_is_unavailable(self) -> None:
        powershell = shutil.which("pwsh") or shutil.which("pwsh.exe")
        if powershell is None:
            self.skipTest("PowerShell is unavailable on this host")

        shutil.copy2(GET_ENV_PS1, self.firmware / "get_env.ps1")
        idf_path = self.make_idf(Path(self.temporary_directory.name) / "external idf")
        script_path = self._powershell_path(self.firmware / "get_env.ps1", powershell)
        configured_path = self._powershell_path(idf_path, powershell)
        activate_path = self._powershell_path(
            idf_path / "tools" / "activate.py", powershell
        )
        export_path = self._powershell_path(idf_path / "export.ps1", powershell)
        self.configure(configured_path, "external")
        escaped_script = script_path.replace("'", "''")
        escaped_export = export_path.replace("'", "''")

        result = subprocess.run(
            [
                powershell,
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-Command",
                "$env:PATH = ''; "
                "function global:python { "
                "$env:AIRDAP_TEST_PYTHON_ARGS = $args -join '|'; "
                "$global:LASTEXITCODE = 0; "
                f"Write-Output '{escaped_export}' }}; "
                f". '{escaped_script}'; "
                "Write-Output \"python=[$env:AIRDAP_TEST_PYTHON_ARGS]\"",
            ],
            check=False,
            text=True,
            capture_output=True,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        expected_args = f"{activate_path}|--export|--shell|powershell"
        self.assertIn(f"python=[{expected_args}]", result.stdout)

    def test_powershell_does_not_fallback_when_uv_fails(self) -> None:
        powershell = shutil.which("pwsh") or shutil.which("pwsh.exe")
        if powershell is None:
            self.skipTest("PowerShell is unavailable on this host")

        shutil.copy2(GET_ENV_PS1, self.firmware / "get_env.ps1")
        idf_path = self.make_idf(Path(self.temporary_directory.name) / "external idf")
        script_path = self._powershell_path(self.firmware / "get_env.ps1", powershell)
        configured_path = self._powershell_path(idf_path, powershell)
        self.configure(configured_path, "external")
        escaped_script = script_path.replace("'", "''")

        result = subprocess.run(
            [
                powershell,
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-Command",
                "function global:uv { $global:LASTEXITCODE = 23 }; "
                "function global:python { "
                "$env:AIRDAP_TEST_PYTHON_CALLED = 'yes' }; "
                "try { "
                f". '{escaped_script}'; Write-Output 'unexpected-success' "
                "} catch { Write-Output \"error=[$($_.Exception.Message)]\" }; "
                "Write-Output \"python=[$env:AIRDAP_TEST_PYTHON_CALLED]\"",
            ],
            check=False,
            text=True,
            capture_output=True,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(
            "error=[get_env.ps1: uv failed to activate ESP-IDF (exit 23)]",
            result.stdout,
        )
        self.assertIn("python=[]", result.stdout)
        self.assertNotIn("unexpected-success", result.stdout)

    def test_powershell_accepts_legacy_one_line_configuration_when_available(
        self,
    ) -> None:
        powershell = shutil.which("pwsh") or shutil.which("pwsh.exe")
        if powershell is None:
            self.skipTest("PowerShell is unavailable on this host")

        shutil.copy2(GET_ENV_PS1, self.firmware / "get_env.ps1")
        idf_path = self.make_idf(Path(self.temporary_directory.name) / "legacy idf")
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
        self.configure(configured_path, "managed")
        escaped_script = script_path.replace("'", "''")

        result = subprocess.run(
            [
                powershell,
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-Command",
                "$env:IDF_PYTHON_ENV_PATH = 'C:\\stale\\python'; "
                "$env:IDF_SKIP_TOOLS_CHECK = 'stale'; "
                "$env:IDF_SKIP_CHECK_SUBMODULES = 'stale'; "
                f". '{escaped_script}'; Write-Output $env:IDF_TOOLS_PATH; "
                "if (Test-Path Env:IDF_PYTHON_ENV_PATH) { "
                "Write-Output $env:IDF_PYTHON_ENV_PATH } else { "
                "Write-Output '<unset>' }; "
                "Write-Output $env:IDF_SKIP_TOOLS_CHECK; "
                "Write-Output $env:IDF_SKIP_CHECK_SUBMODULES",
            ],
            check=False,
            text=True,
            capture_output=True,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        output_lines = result.stdout.strip().splitlines()
        self.assertEqual(
            output_lines[-4].casefold(),
            expected_tools_path.casefold(),
        )
        self.assertEqual(output_lines[-3], "<unset>")
        self.assertEqual(output_lines[-2], "1")
        self.assertEqual(output_lines[-1], "1")

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
