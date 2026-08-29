from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[2] / "hil" / "wired_smoke.py"
SPEC = importlib.util.spec_from_file_location("wired_smoke", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
wired_smoke = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = wired_smoke
SPEC.loader.exec_module(wired_smoke)


class FakeDap:
    def __init__(self, packet_size: int = wired_smoke.DAP_PACKET_SIZE, status: int = 1):
        self.packet_size = packet_size
        self.status = status
        self.idcode = 0x2BA01477
        self.commands: list[int] = []
        self.max_block_followed_by_info = False
        self._saw_max_block = False

    @staticmethod
    def _info(payload: bytes) -> bytes:
        return bytes((wired_smoke.ID_DAP_INFO, len(payload))) + payload

    def exchange(self, request: bytes) -> bytes:
        self.commands.append(request[0])
        if self._saw_max_block:
            self.max_block_followed_by_info = (
                request == bytes((wired_smoke.ID_DAP_INFO, wired_smoke.DAP_INFO_PACKET_SIZE))
            )
            self._saw_max_block = False

        if request[0] == wired_smoke.ID_DAP_INFO:
            values = {
                wired_smoke.DAP_INFO_VENDOR: b"AirDAP\0",
                wired_smoke.DAP_INFO_PRODUCT: b"AirDAP CMSIS-DAP v2\0",
                wired_smoke.DAP_INFO_SERIAL: b"ADP-001122334455\0",
                wired_smoke.DAP_INFO_DAP_FIRMWARE: b"2.1.2\0",
                wired_smoke.DAP_INFO_PRODUCT_FIRMWARE: b"0.1.0\0",
                wired_smoke.DAP_INFO_CAPABILITIES: b"\x01\x01",
                wired_smoke.DAP_INFO_PACKET_COUNT: b"\x01",
                wired_smoke.DAP_INFO_PACKET_SIZE: self.packet_size.to_bytes(2, "little"),
            }
            return self._info(values[request[1]])
        if request[0] == wired_smoke.ID_DAP_CONNECT:
            return bytes((wired_smoke.ID_DAP_CONNECT, 1))
        if request[0] in (
            wired_smoke.ID_DAP_TRANSFER_CONFIGURE,
            wired_smoke.ID_DAP_SWJ_CLOCK,
            wired_smoke.ID_DAP_SWJ_SEQUENCE,
            wired_smoke.ID_DAP_SWD_CONFIGURE,
        ):
            return bytes((request[0], 0))
        if request[0] == wired_smoke.ID_DAP_TRANSFER_BLOCK:
            count = int.from_bytes(request[2:4], "little")
            if count == wired_smoke.DAP_MAX_BLOCK_READS:
                self._saw_max_block = True
            return (
                bytes((request[0], count & 0xFF, count >> 8, self.status))
                + self.idcode.to_bytes(4, "little") * count
            )
        if request[0] == wired_smoke.ID_DAP_RESET_TARGET:
            return bytes((request[0], 0, 0))
        if request[0] == wired_smoke.ID_DAP_DISCONNECT:
            return bytes((request[0], 0))
        raise AssertionError(f"unexpected request {request.hex()}")


class FakeSerialModule:
    EIGHTBITS = 8
    SEVENBITS = 7
    FIVEBITS = 5
    PARITY_NONE = "N"
    PARITY_EVEN = "E"
    PARITY_ODD = "O"
    STOPBITS_ONE = 1
    STOPBITS_TWO = 2
    STOPBITS_ONE_POINT_FIVE = 1.5
    configurations: list[dict[str, object]] = []

    class Serial:
        def __init__(self, **kwargs: object):
            FakeSerialModule.configurations.append(kwargs)
            self.bytesize = int(kwargs["bytesize"])
            self.buffer = bytearray()

        def __enter__(self) -> "FakeSerialModule.Serial":
            return self

        def __exit__(self, *args: object) -> None:
            del args

        def reset_input_buffer(self) -> None:
            self.buffer.clear()

        def reset_output_buffer(self) -> None:
            pass

        def write(self, data: bytes) -> int:
            mask = (1 << self.bytesize) - 1
            self.buffer.extend(value & mask for value in data)
            return len(data)

        def flush(self) -> None:
            pass

        def read(self, length: int) -> bytes:
            result = bytes(self.buffer[:length])
            del self.buffer[:length]
            return result


class WiredSmokeTests(unittest.TestCase):
    def test_collects_probe_idcode_boundary_and_reset_evidence(self) -> None:
        dap = FakeDap()

        evidence = wired_smoke.collect_dap_evidence(
            dap.exchange,
            clock_hz=500000,
            idcode_reads=130,
            exercise_reset=True,
        )

        self.assertEqual(evidence["probe"]["packet_size"], 508)
        self.assertEqual(evidence["idcode"], "0x2BA01477")
        self.assertEqual(evidence["idcode_reads"], 130)
        self.assertTrue(evidence["packet_boundary_508_checked"])
        self.assertTrue(evidence["reset_command_checked"])
        self.assertTrue(dap.max_block_followed_by_info)
        self.assertEqual(dap.commands[-1], wired_smoke.ID_DAP_DISCONNECT)

    def test_rejects_wrong_packet_size(self) -> None:
        dap = FakeDap(packet_size=512)

        with self.assertRaisesRegex(wired_smoke.VerificationError, "packet size"):
            wired_smoke.collect_dap_evidence(dap.exchange, 500000, 126, False)

    def test_rejects_non_ok_transfer_status_and_disconnects(self) -> None:
        dap = FakeDap(status=2)

        with self.assertRaisesRegex(wired_smoke.VerificationError, "ACK OK"):
            wired_smoke.collect_dap_evidence(dap.exchange, 500000, 126, False)
        self.assertEqual(dap.commands[-1], wired_smoke.ID_DAP_DISCONNECT)

    def test_readback_comparison_checks_all_chunks(self) -> None:
        image = bytes(index & 0xFF for index in range(2500))
        calls: list[tuple[int, int]] = []

        def read(address: int, length: int) -> list[int]:
            calls.append((address, length))
            offset = address - 0x08000000
            return list(image[offset:offset + length])

        wired_smoke.compare_target_bytes(read, 0x08000000, image)
        self.assertEqual(calls, [
            (0x08000000, 1024),
            (0x08000400, 1024),
            (0x08000800, 452),
        ])

    def test_readback_comparison_reports_mismatch_address(self) -> None:
        image = b"\xAA" * 32

        def read(address: int, length: int) -> list[int]:
            del address
            actual = bytearray(image[:length])
            actual[7] = 0x55
            return list(actual)

        with self.assertRaisesRegex(wired_smoke.VerificationError, "0x08000007"):
            wired_smoke.compare_target_bytes(read, 0x08000000, image)

    def test_frequency_parser(self) -> None:
        self.assertEqual(wired_smoke.parse_frequencies("500000,0xF4240"), [500000, 1000000])

    def test_uart_loopback_uses_values_representable_by_each_word_length(self) -> None:
        FakeSerialModule.configurations.clear()

        evidence = wired_smoke.run_uart_loopback("loopback", FakeSerialModule)

        self.assertEqual(len(evidence["cases"]), 4)
        self.assertEqual(
            [item["bytesize"] for item in FakeSerialModule.configurations],
            [8, 7, 8, 5],
        )

    def test_main_records_unexpected_backend_failure(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "evidence.json"
            with mock.patch.object(
                    wired_smoke,
                    "open_usb_transport",
                    side_effect=RuntimeError("backend failed"),
            ):
                result = wired_smoke.main(["--output", str(output)])

            evidence = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(result, 1)
            self.assertEqual(evidence["status"], "failed")
            self.assertEqual(evidence["error_type"], "RuntimeError")
            self.assertEqual(evidence["error"], "backend failed")


if __name__ == "__main__":
    unittest.main()
