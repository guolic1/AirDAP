from __future__ import annotations

import subprocess
import tempfile
import unittest
from pathlib import Path


MODULE = Path(__file__).resolve().parents[3] / "cmake" / "airdap_project_version.cmake"


def run(*args: str, cwd: Path, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        args,
        cwd=cwd,
        check=check,
        text=True,
        capture_output=True,
    )


class ProjectVersionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary_directory = tempfile.TemporaryDirectory()
        self.repo = Path(self.temporary_directory.name)
        run("git", "init", "--quiet", cwd=self.repo)
        run("git", "config", "user.name", "AirDAP Test", cwd=self.repo)
        run("git", "config", "user.email", "airdap-test@example.invalid", cwd=self.repo)
        self.commit("initial")

    def tearDown(self) -> None:
        self.temporary_directory.cleanup()

    def commit(self, content: str) -> None:
        (self.repo / "content.txt").write_text(content, encoding="utf-8")
        run("git", "add", "content.txt", cwd=self.repo)
        run("git", "commit", "--quiet", "-m", content, cwd=self.repo)

    def resolve(self) -> subprocess.CompletedProcess[str]:
        script = self.repo / "resolve.cmake"
        script.write_text(
            f'include("{MODULE.as_posix()}")\n'
            'airdap_resolve_project_version("${REPO_DIR}" resolved_version)\n'
            'message("AIRDAP_PROJECT_VERSION=${resolved_version}")\n',
            encoding="utf-8",
        )
        return run(
            "cmake",
            f"-DREPO_DIR={self.repo}",
            "-P",
            str(script),
            cwd=self.repo,
            check=False,
        )

    def configure_project(self, build_directory: Path) -> None:
        (self.repo / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.22)\n"
            "project(airdap_version_fixture NONE)\n"
            f'include("{MODULE.as_posix()}")\n'
            'airdap_resolve_project_version("${CMAKE_SOURCE_DIR}" resolved_version)\n'
            'file(WRITE "${CMAKE_BINARY_DIR}/resolved-version.txt" '
            '"${resolved_version}\\n")\n',
            encoding="utf-8",
        )
        run(
            "cmake",
            "-S",
            str(self.repo),
            "-B",
            str(build_directory),
            cwd=self.repo,
        )

    def test_untagged_head_uses_short_hash(self) -> None:
        expected = run(
            "git", "rev-parse", "--short=7", "HEAD", cwd=self.repo
        ).stdout.strip()

        result = self.resolve()

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"AIRDAP_PROJECT_VERSION={expected}", result.stderr)

    def test_exact_tag_uses_tag_name(self) -> None:
        run("git", "tag", "v1.2.3", cwd=self.repo)

        result = self.resolve()

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("AIRDAP_PROJECT_VERSION=v1.2.3", result.stderr)

    def test_tag_on_earlier_commit_is_not_inherited(self) -> None:
        run("git", "tag", "v1.2.3", cwd=self.repo)
        self.commit("next")
        expected = run(
            "git", "rev-parse", "--short=7", "HEAD", cwd=self.repo
        ).stdout.strip()

        result = self.resolve()

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(f"AIRDAP_PROJECT_VERSION={expected}", result.stderr)
        self.assertNotIn("AIRDAP_PROJECT_VERSION=v1.2.3", result.stderr)

    def test_multiple_exact_tags_are_rejected(self) -> None:
        run("git", "tag", "v1.2.3", cwd=self.repo)
        run("git", "tag", "release-1.2.3", cwd=self.repo)

        result = self.resolve()

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("multiple tags", result.stderr)

    def test_incremental_build_tracks_commit_and_tag_changes(self) -> None:
        build_directory = self.repo / "build"
        self.configure_project(build_directory)
        version_file = build_directory / "resolved-version.txt"
        initial_hash = run(
            "git", "rev-parse", "--short=7", "HEAD", cwd=self.repo
        ).stdout.strip()
        self.assertEqual(version_file.read_text(encoding="utf-8").strip(), initial_hash)

        self.commit("next")
        next_hash = run(
            "git", "rev-parse", "--short=7", "HEAD", cwd=self.repo
        ).stdout.strip()
        run("cmake", "--build", str(build_directory), cwd=self.repo)
        self.assertEqual(version_file.read_text(encoding="utf-8").strip(), next_hash)

        run("git", "tag", "v2.0.0", cwd=self.repo)
        run("cmake", "--build", str(build_directory), cwd=self.repo)
        self.assertEqual(version_file.read_text(encoding="utf-8").strip(), "v2.0.0")


if __name__ == "__main__":
    unittest.main()
