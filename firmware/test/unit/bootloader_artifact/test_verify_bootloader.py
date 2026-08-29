from __future__ import annotations

import importlib.util
import unittest
from pathlib import Path


SCRIPT = Path(__file__).resolve().parents[3] / "tools" / "verify_bootloader.py"
SPEC = importlib.util.spec_from_file_location("verify_bootloader", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
verify_bootloader = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(verify_bootloader)


VALID_CONFIG = """
CONFIG_IDF_TARGET="esp32s3"
CONFIG_BOOTLOADER_REGION_PROTECTION_ENABLE=y
CONFIG_BOOTLOADER_WDT_ENABLE=y
CONFIG_BOOTLOADER_WDT_TIME_MS=9000
# CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP is not set
# CONFIG_BOOTLOADER_SKIP_VALIDATE_ON_POWER_ON is not set
# CONFIG_BOOTLOADER_SKIP_VALIDATE_ALWAYS is not set
"""

VALID_NM = """
403c89cc T bootloader_before_init
403c9718 T bootloader_hooks_include
"""

VALID_DISASSEMBLY = """
403c8934 <call_start_cpu0>:
403c893d: 0008e5 call8 403c89cc <bootloader_before_init>
403c8940: 003c25 call8 403c8d04 <bootloader_init>
403c8945: 0478a5 call8 403cd0d0 <bootloader_reset>
"""


class BootloaderArtifactTests(unittest.TestCase):
    def test_accepts_required_config_symbols_and_call_order(self) -> None:
        config = verify_bootloader.parse_sdkconfig(VALID_CONFIG)

        verify_bootloader.validate_config(config)
        verify_bootloader.validate_symbols(VALID_NM)
        verify_bootloader.validate_call_order(VALID_DISASSEMBLY)

    def test_rejects_disabled_safety_config(self) -> None:
        for line, message in (
            ("CONFIG_BOOTLOADER_REGION_PROTECTION_ENABLE=n", "region protection"),
            ("CONFIG_BOOTLOADER_WDT_ENABLE=n", "watchdog"),
            ("CONFIG_BOOTLOADER_WDT_TIME_MS=1", "9000"),
        ):
            with self.subTest(line=line):
                config = verify_bootloader.parse_sdkconfig(VALID_CONFIG + line + "\n")
                with self.assertRaisesRegex(verify_bootloader.VerificationError, message):
                    verify_bootloader.validate_config(config)

    def test_rejects_every_image_validation_bypass(self) -> None:
        for option in verify_bootloader.FORBIDDEN_VALIDATION_BYPASSES:
            with self.subTest(option=option):
                config = verify_bootloader.parse_sdkconfig(
                    VALID_CONFIG + f"{option}=y\n"
                )
                with self.assertRaisesRegex(verify_bootloader.VerificationError, option):
                    verify_bootloader.validate_config(config)

    def test_rejects_wrong_target(self) -> None:
        config = verify_bootloader.parse_sdkconfig(
            VALID_CONFIG + 'CONFIG_IDF_TARGET="esp32"\n'
        )

        with self.assertRaisesRegex(verify_bootloader.VerificationError, "ESP32-S3"):
            verify_bootloader.validate_config(config)

    def test_rejects_weak_or_missing_hook_symbols(self) -> None:
        with self.assertRaisesRegex(verify_bootloader.VerificationError, "strong text symbol"):
            verify_bootloader.validate_symbols(
                "403c89cc W bootloader_before_init\n"
                "403c9718 T bootloader_hooks_include\n"
            )
        with self.assertRaisesRegex(verify_bootloader.VerificationError, "bootloader_hooks_include"):
            verify_bootloader.validate_symbols(
                "403c89cc T bootloader_before_init\n"
            )

    def test_rejects_hook_called_after_bootloader_initialization(self) -> None:
        disassembly = """
403c8934 <call_start_cpu0>:
403c8940: 003c25 call8 403c8d04 <bootloader_init>
403c8943: 0008e5 call8 403c89cc <bootloader_before_init>
"""

        with self.assertRaisesRegex(verify_bootloader.VerificationError, "before bootloader_init"):
            verify_bootloader.validate_call_order(disassembly)

    def test_rejects_missing_entrypoint_disassembly(self) -> None:
        with self.assertRaisesRegex(verify_bootloader.VerificationError, "call_start_cpu0"):
            verify_bootloader.validate_call_order(
                "403c89cc <bootloader_before_init>:\n"
            )


if __name__ == "__main__":
    unittest.main()
