from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[3] / "tools" / "verify_ota_layout.py"
SPEC = importlib.util.spec_from_file_location("verify_ota_layout", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
verify_ota_layout = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(verify_ota_layout)


VALID_PARTITIONS = """
# Name,   Type, SubType, Offset,   Size,      Flags
nvs,      data, nvs,     0x9000,   0x6000,
otadata,  data, ota,     0xF000,   0x2000,
phy_init, data, phy,     0x11000,  0x1000,
ota_0,    app,  ota_0,   0x20000,  0x3F0000,
ota_1,    app,  ota_1,   0x410000, 0x3F0000,
"""

VALID_CONFIG = """
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHSIZE_8MB=y
CONFIG_ESPTOOLPY_FLASHSIZE="8MB"
CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"
"""


class OtaLayoutTests(unittest.TestCase):
    def test_accepts_exact_8mb_ab_layout_and_config(self) -> None:
        verify_ota_layout.validate_config(
            verify_ota_layout.parse_sdkconfig(VALID_CONFIG)
        )
        verify_ota_layout.validate_layout(
            verify_ota_layout.parse_partition_csv(VALID_PARTITIONS)
        )

    def test_accepts_binary_size_suffixes(self) -> None:
        self.assertEqual(verify_ota_layout.parse_size("4K"), 4 * 1024)
        self.assertEqual(verify_ota_layout.parse_size("4M"), 4 * 1024 * 1024)
        self.assertEqual(verify_ota_layout.parse_size("0x2000"), 0x2000)

    def test_rejects_wrong_flash_or_disabled_rollback(self) -> None:
        for replacement, message in (
            ("CONFIG_ESPTOOLPY_FLASHSIZE_8MB=n", "8 MiB"),
            ('CONFIG_ESPTOOLPY_FLASHSIZE="16MB"', "8MB"),
            ("CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=n", "rollback"),
        ):
            with self.subTest(replacement=replacement):
                lines = [
                    replacement if line.startswith(replacement.split("=", 1)[0] + "=") else line
                    for line in VALID_CONFIG.splitlines()
                ]
                with self.assertRaisesRegex(
                    verify_ota_layout.VerificationError,
                    message,
                ):
                    verify_ota_layout.validate_config(
                        verify_ota_layout.parse_sdkconfig("\n".join(lines))
                    )

    def test_rejects_factory_or_missing_ota_partition(self) -> None:
        with self.assertRaisesRegex(verify_ota_layout.VerificationError, "factory"):
            verify_ota_layout.validate_layout(
                verify_ota_layout.parse_partition_csv(
                    VALID_PARTITIONS + "factory,app,factory,0x800000,1M,\n"
                )
            )

        with self.assertRaisesRegex(
            verify_ota_layout.VerificationError,
            "unexpected app partition",
        ):
            verify_ota_layout.validate_layout(
                verify_ota_layout.parse_partition_csv(
                    VALID_PARTITIONS + "ota_2,app,ota_2,0x800000,4M,\n"
                )
            )

        with self.assertRaisesRegex(verify_ota_layout.VerificationError, "ota_1"):
            verify_ota_layout.validate_layout(
                verify_ota_layout.parse_partition_csv(
                    VALID_PARTITIONS.replace(
                        "ota_1,    app,  ota_1,   0x410000, 0x3F0000,\n",
                        "",
                    )
                )
            )

    def test_rejects_changed_required_offset_or_size(self) -> None:
        for old, new, message in (
            ("0x20000,  0x3F0000", "0x30000,  0x3F0000", "ota_0"),
            ("0x410000, 0x3F0000", "0x410000, 0x3E0000", "ota_1"),
            ("0xF000,   0x2000", "0xE000,   0x2000", "otadata"),
        ):
            with self.subTest(new=new):
                with self.assertRaisesRegex(
                    verify_ota_layout.VerificationError,
                    message,
                ):
                    verify_ota_layout.validate_layout(
                        verify_ota_layout.parse_partition_csv(
                            VALID_PARTITIONS.replace(old, new)
                        )
                    )

    def test_rejects_overlap_and_flash_overflow_for_extra_partitions(self) -> None:
        for extra, message in (
            ("storage,data,fat,0x400000,0x20000,\n", "overlap"),
            ("storage,data,fat,0x7F0000,0x20000,\n", "8 MiB"),
        ):
            with self.subTest(extra=extra):
                with self.assertRaisesRegex(
                    verify_ota_layout.VerificationError,
                    message,
                ):
                    verify_ota_layout.validate_layout(
                        verify_ota_layout.parse_partition_csv(
                            VALID_PARTITIONS + extra
                        )
                    )


if __name__ == "__main__":
    unittest.main()
