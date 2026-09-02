from __future__ import annotations

import importlib.util
import io
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[3] / "tools" / "setup.py"


def load_setup_module():
    spec = importlib.util.spec_from_file_location("airdap_setup", SCRIPT)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def make_fake_idf(path: Path, version: tuple[int, int, int] = (6, 1, 0)) -> Path:
    cmake = path / "tools" / "cmake"
    cmake.mkdir(parents=True)
    (cmake / "project.cmake").write_text("# fake project\n", encoding="utf-8")
    (cmake / "version.cmake").write_text(
        f"set(IDF_VERSION_MAJOR {version[0]})\n"
        f"set(IDF_VERSION_MINOR {version[1]})\n"
        f"set(IDF_VERSION_PATCH {version[2]})\n",
        encoding="utf-8",
    )
    for relative in (
        "tools/idf.py",
        "tools/idf_tools.py",
        "tools/activate.py",
        "export.sh",
        "export.ps1",
    ):
        file_path = path / relative
        file_path.parent.mkdir(parents=True, exist_ok=True)
        file_path.write_text("# fake\n", encoding="utf-8")
    return path


class SetupTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.root = Path(self.temporary_directory.name)
        self.setup = load_setup_module()

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def test_external_idf_path_is_validated_resolved_and_saved(self) -> None:
        idf_path = make_fake_idf(self.root / "external idf")
        config_file = self.root / "state" / "idf-path.txt"
        installer = mock.Mock()

        configured = self.setup.configure_environment(
            idf_path,
            config_file=config_file,
            default_idf_path=self.root / "managed" / "esp-idf",
            tools_path=self.root / "managed",
            install_external=installer,
        )

        installer.assert_called_once_with(idf_path.resolve(), self.root / "managed")
        self.assertEqual(configured, idf_path.resolve())
        self.assertEqual(
            config_file.read_text(encoding="utf-8"),
            str(idf_path.resolve()) + "\n",
        )

    def test_external_idf_path_must_be_version_6_1_0(self) -> None:
        idf_path = make_fake_idf(self.root / "wrong-idf", (6, 0, 2))

        with self.assertRaisesRegex(self.setup.SetupError, "6.1.0"):
            self.setup.validate_idf_path(idf_path)

    def test_external_idf_path_requires_activation_and_build_files(self) -> None:
        idf_path = self.root / "incomplete-idf"
        idf_path.mkdir()

        with self.assertRaisesRegex(self.setup.SetupError, "missing"):
            self.setup.validate_idf_path(idf_path)

    def test_no_argument_installs_managed_environment_before_saving(self) -> None:
        default_idf = make_fake_idf(self.root / "managed" / "esp-idf")
        config_file = self.root / "managed" / "idf-path.txt"
        installer = mock.Mock()

        configured = self.setup.configure_environment(
            None,
            config_file=config_file,
            default_idf_path=default_idf,
            tools_path=self.root / "managed",
            install_managed=installer,
        )

        installer.assert_called_once_with(default_idf, self.root / "managed")
        self.assertEqual(configured, default_idf.resolve())
        self.assertEqual(
            config_file.read_text(encoding="utf-8"),
            str(default_idf.resolve()) + "\n",
        )

    def test_failed_managed_install_preserves_previous_configuration(self) -> None:
        previous = make_fake_idf(self.root / "previous-idf")
        config_file = self.root / "state" / "idf-path.txt"
        config_file.parent.mkdir()
        config_file.write_text(str(previous.resolve()) + "\n", encoding="utf-8")

        def fail_install(idf_path: Path, tools_path: Path) -> None:
            del idf_path, tools_path
            raise self.setup.SetupError("download failed")

        with self.assertRaisesRegex(self.setup.SetupError, "download failed"):
            self.setup.configure_environment(
                None,
                config_file=config_file,
                default_idf_path=self.root / "managed" / "esp-idf",
                tools_path=self.root / "managed",
                install_managed=fail_install,
            )

        self.assertEqual(
            config_file.read_text(encoding="utf-8"),
            str(previous.resolve()) + "\n",
        )

    def test_managed_install_uses_pinned_commit_and_repository_local_tools(self) -> None:
        idf_path = self.root / "managed" / "esp-idf"
        tools_path = self.root / "managed"
        commands: list[tuple[list[str], Path | None, dict[str, str]]] = []

        def fake_run(
            command: list[str],
            *,
            cwd: Path | None = None,
            env: dict[str, str] | None = None,
            capture_output: bool = False,
        ) -> str | None:
            del capture_output
            commands.append((command, cwd, dict(env or {})))
            if command[:2] == ["git", "clone"]:
                make_fake_idf(Path(command[-1]))
            elif command[:2] == ["git", "-C"] and command[-2:] == ["rev-parse", "HEAD"]:
                return self.setup.DEFAULT_IDF_COMMIT
            return None

        self.setup.install_managed_environment(
            idf_path,
            tools_path,
            run_command=fake_run,
        )

        clone = commands[0][0]
        self.assertEqual(clone[:4], ["git", "clone", "--branch", "v6.1"])
        self.assertNotIn("--recursive", clone)
        self.assertTrue(
            any(
                command[:4]
                == ["git", "-C", str(idf_path.resolve()), "submodule"]
                and command[4:] == ["update", "--init", "--recursive", "--depth", "1"]
                for command, _, _ in commands
            ),
            commands,
        )

        tool_commands = [command for command, _, _ in commands if "idf_tools.py" in " ".join(command)]
        self.assertTrue(
            any(
                command[-4:] == ["--targets=esp32s3", "required", "cmake", "ninja"]
                for command in tool_commands
            ),
            tool_commands,
        )
        self.assertTrue(
            any(
                command[-2:] == ["install-python-env", "--features=core"]
                for command in tool_commands
            ),
            tool_commands,
        )
        for command, _, environment in commands:
            if "idf_tools.py" in " ".join(command):
                self.assertEqual(environment["IDF_PATH"], str(idf_path.resolve()))
                self.assertEqual(environment["IDF_TOOLS_PATH"], str(tools_path.resolve()))
                self.assertNotIn("IDF_PYTHON_ENV_PATH", environment)

    def test_existing_managed_checkout_retries_submodule_update(self) -> None:
        idf_path = make_fake_idf(self.root / "managed" / "esp-idf")
        commands: list[list[str]] = []

        def fake_run(
            command: list[str],
            *,
            cwd: Path | None = None,
            env: dict[str, str] | None = None,
            capture_output: bool = False,
        ) -> str | None:
            del cwd, env, capture_output
            commands.append(command)
            if command[-2:] == ["rev-parse", "HEAD"]:
                return self.setup.DEFAULT_IDF_COMMIT
            return None

        self.setup.install_managed_environment(
            idf_path,
            self.root / "managed",
            run_command=fake_run,
        )

        self.assertIn(
            [
                "git",
                "-C",
                str(idf_path.resolve()),
                "submodule",
                "update",
                "--init",
                "--recursive",
                "--depth",
                "1",
            ],
            commands,
        )

    def test_cli_accepts_zero_or_one_path_argument(self) -> None:
        parser = self.setup.make_parser()

        self.assertIsNone(parser.parse_args([]).idf_path)
        self.assertEqual(parser.parse_args(["relative/idf"]).idf_path, Path("relative/idf"))
        with mock.patch("sys.stderr", new=io.StringIO()):
            with self.assertRaises(SystemExit):
                parser.parse_args(["one", "two"])

    def test_cli_reports_setup_errors_without_traceback(self) -> None:
        result = subprocess.run(
            [sys.executable, str(SCRIPT), str(self.root / "missing")],
            check=False,
            text=True,
            capture_output=True,
            env={**os.environ, "NO_COLOR": "1"},
        )

        self.assertEqual(result.returncode, 1)
        self.assertRegex(result.stderr, r"^setup\.py: .+missing")
        self.assertNotIn("Traceback", result.stderr)


if __name__ == "__main__":
    unittest.main()
