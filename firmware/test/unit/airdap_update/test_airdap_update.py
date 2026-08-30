from __future__ import annotations

import importlib.util
import io
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[3] / "tools" / "airdap-update.py"
SPEC = importlib.util.spec_from_file_location("airdap_update", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
airdap_update = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = airdap_update
SPEC.loader.exec_module(airdap_update)


class FakeEndpoint:
    def __init__(
        self,
        address: int,
        reads: list[bytes | BaseException] | None = None,
    ):
        self.bEndpointAddress = address
        self.bmAttributes = 2
        self.wMaxPacketSize = 64
        self.reads = list(reads or [])
        self.writes: list[bytes] = []

    def write(self, data: bytes, timeout: int) -> int:
        del timeout
        self.writes.append(bytes(data))
        return len(data)

    def read(self, size: int, timeout: int) -> bytes:
        del size, timeout
        if not self.reads:
            raise AssertionError("unexpected Bulk IN read")
        result = self.reads.pop(0)
        if isinstance(result, BaseException):
            raise result
        return result


class FakeInterface:
    bInterfaceClass = 0xFF
    bInterfaceSubClass = 0
    bInterfaceProtocol = 0

    def __init__(self, endpoints: list[FakeEndpoint]):
        self.endpoints = endpoints

    def __iter__(self):
        return iter(self.endpoints)


class FakeConfiguration:
    def __init__(self, interface: FakeInterface):
        self.interface = interface

    def __getitem__(self, key: tuple[int, int]) -> FakeInterface:
        if key != (airdap_update.DAP_INTERFACE, 0):
            raise KeyError(key)
        return self.interface


class FakeDevice:
    iSerialNumber = 3

    def __init__(self, interface: FakeInterface, serial: str = "ADP-001122334455"):
        self.configuration = FakeConfiguration(interface)
        self.serial = serial
        self.detached: list[int] = []
        self.attached: list[int] = []
        self.set_configuration_calls = 0

    def get_active_configuration(self) -> FakeConfiguration:
        return self.configuration

    def set_configuration(self) -> None:
        self.set_configuration_calls += 1

    def is_kernel_driver_active(self, interface: int) -> bool:
        return interface == airdap_update.DAP_INTERFACE

    def detach_kernel_driver(self, interface: int) -> None:
        self.detached.append(interface)

    def attach_kernel_driver(self, interface: int) -> None:
        self.attached.append(interface)


class FakeUsbUtil:
    def __init__(self):
        self.claimed: list[tuple[FakeDevice, int]] = []
        self.released: list[tuple[FakeDevice, int]] = []
        self.disposed: list[FakeDevice] = []

    def claim_interface(self, device: FakeDevice, interface: int) -> None:
        self.claimed.append((device, interface))

    def release_interface(self, device: FakeDevice, interface: int) -> None:
        self.released.append((device, interface))

    def dispose_resources(self, device: FakeDevice) -> None:
        self.disposed.append(device)

    @staticmethod
    def get_string(device: FakeDevice, index: int) -> str:
        assert index == device.iSerialNumber
        return device.serial


def status(command: int, value: int = 0) -> bytes:
    return bytes((command, value))


def query(version: str = "1.2.3") -> bytes:
    encoded = version.encode("utf-8")
    return bytes((airdap_update.OTA_QUERY, 0, 1, 1)) + struct.pack(
        "<I", 0x400000
    ) + bytes((len(encoded),)) + encoded


class AirDapUpdateTests(unittest.TestCase):
    def make_transport(
        self,
        responses: list[bytes | BaseException],
    ) -> tuple[object, FakeEndpoint, FakeEndpoint, FakeDevice, FakeUsbUtil]:
        endpoint_out = FakeEndpoint(airdap_update.DAP_OUT_ENDPOINT)
        endpoint_in = FakeEndpoint(airdap_update.DAP_IN_ENDPOINT, responses)
        device = FakeDevice(FakeInterface([endpoint_out, endpoint_in]))
        usb_util = FakeUsbUtil()
        transport = airdap_update.DapOtaTransport(
            device,
            usb_util,
            timeout_ms=2500,
        )
        return transport, endpoint_out, endpoint_in, device, usb_util

    def test_claims_only_existing_dap_interface_without_reconfiguring(self) -> None:
        transport, _, _, device, usb_util = self.make_transport([])

        transport.open()
        transport.close()

        self.assertEqual(device.set_configuration_calls, 0)
        self.assertEqual(device.detached, [airdap_update.DAP_INTERFACE])
        self.assertEqual(usb_util.claimed, [(device, airdap_update.DAP_INTERFACE)])
        self.assertEqual(usb_util.released, [(device, airdap_update.DAP_INTERFACE)])
        self.assertEqual(device.attached, [airdap_update.DAP_INTERFACE])
        self.assertEqual(usb_util.disposed, [device])

    def test_close_tolerates_expected_usb_disappearance_after_reboot(self) -> None:
        class FakeUsbError(Exception):
            pass

        class ErrorCore:
            USBError = FakeUsbError

        class DisconnectedUsbUtil(FakeUsbUtil):
            def dispose_resources(self, device: FakeDevice) -> None:
                del device
                raise FakeUsbError("device rebooted")

        endpoint_out = FakeEndpoint(airdap_update.DAP_OUT_ENDPOINT)
        endpoint_in = FakeEndpoint(airdap_update.DAP_IN_ENDPOINT)
        device = FakeDevice(FakeInterface([endpoint_out, endpoint_in]))
        transport = airdap_update.DapOtaTransport(
            device,
            DisconnectedUsbUtil(),
            usb_core=ErrorCore,
        )
        transport.open()

        transport.close()

    def test_upload_orders_disconnect_chunks_commit_and_no_response_reboot(self) -> None:
        image = bytes(index & 0xFF for index in range(500))
        transport, endpoint_out, endpoint_in, _, _ = self.make_transport([
            query("old-version"),
            status(airdap_update.DAP_DISCONNECT),
            status(airdap_update.OTA_BEGIN),
            status(airdap_update.OTA_WRITE) + struct.pack("<I", 496),
            status(airdap_update.OTA_WRITE) + struct.pack("<I", 500),
            status(airdap_update.OTA_COMMIT),
        ])
        transport.open()

        before = airdap_update.upload_image(transport, io.BytesIO(image), len(image))

        self.assertEqual(before.running_version, "old-version")
        self.assertEqual(
            [request[0] for request in endpoint_out.writes],
            [
                airdap_update.OTA_QUERY,
                airdap_update.DAP_DISCONNECT,
                airdap_update.OTA_BEGIN,
                airdap_update.OTA_WRITE,
                airdap_update.OTA_WRITE,
                airdap_update.OTA_COMMIT,
                airdap_update.OTA_REBOOT,
            ],
        )
        self.assertEqual(endpoint_out.writes[2], bytes((airdap_update.OTA_BEGIN,)) + struct.pack("<I", 500))
        self.assertEqual(endpoint_out.writes[3][1:7], struct.pack("<IH", 0, 496))
        self.assertEqual(endpoint_out.writes[3][7:], image[:496])
        self.assertEqual(endpoint_out.writes[4][1:7], struct.pack("<IH", 496, 4))
        self.assertEqual(endpoint_out.writes[4][7:], image[496:])
        self.assertEqual(endpoint_in.reads, [])

    def test_failed_write_attempts_abort_and_never_commits(self) -> None:
        transport, endpoint_out, _, _, _ = self.make_transport([
            query(),
            status(airdap_update.DAP_DISCONNECT),
            status(airdap_update.OTA_BEGIN),
            status(airdap_update.OTA_WRITE, 6),
            status(airdap_update.OTA_ABORT),
        ])
        transport.open()

        with mock.patch.object(airdap_update.time, "sleep"):
            with self.assertRaisesRegex(airdap_update.UpdateError, "write failed"):
                airdap_update.upload_image(transport, io.BytesIO(b"firmware"), 8)

        self.assertEqual(
            [request[0] for request in endpoint_out.writes],
            [
                airdap_update.OTA_QUERY,
                airdap_update.DAP_DISCONNECT,
                airdap_update.OTA_BEGIN,
                airdap_update.OTA_WRITE,
                airdap_update.OTA_ABORT,
            ],
        )

    def test_lost_begin_response_still_attempts_abort(self) -> None:
        transport, endpoint_out, endpoint_in, _, _ = self.make_transport([
            query(),
            status(airdap_update.DAP_DISCONNECT),
            RuntimeError("response lost"),
            status(airdap_update.OTA_BEGIN),
            status(airdap_update.OTA_ABORT),
        ])
        transport.open()

        with mock.patch.object(airdap_update.time, "sleep") as sleep:
            with self.assertRaisesRegex(airdap_update.UpdateError, "Bulk IN"):
                airdap_update.upload_image(transport, io.BytesIO(b"firmware"), 8)

        sleep.assert_called_once_with(airdap_update.DAP_STREAM_RECOVERY_SECONDS)

        self.assertEqual(
            [request[0] for request in endpoint_out.writes],
            [
                airdap_update.OTA_QUERY,
                airdap_update.DAP_DISCONNECT,
                airdap_update.OTA_BEGIN,
                airdap_update.OTA_ABORT,
            ],
        )
        self.assertEqual(endpoint_in.reads, [])

    def test_rejects_empty_oversized_and_short_image_streams(self) -> None:
        transport, endpoint_out, _, _, _ = self.make_transport([
            query(),
            query(),
            status(airdap_update.DAP_DISCONNECT),
            status(airdap_update.OTA_BEGIN),
            status(airdap_update.OTA_WRITE) + struct.pack("<I", 4),
            status(airdap_update.OTA_ABORT),
        ])
        transport.open()

        with mock.patch.object(airdap_update.time, "sleep"):
            with self.assertRaisesRegex(airdap_update.UpdateError, "empty"):
                airdap_update.upload_image(transport, io.BytesIO(), 0)
            with self.assertRaisesRegex(airdap_update.UpdateError, "exceeds"):
                airdap_update.upload_image(transport, io.BytesIO(b"x"), 0x400001)
            with self.assertRaisesRegex(airdap_update.UpdateError, "ended"):
                airdap_update.upload_image(transport, io.BytesIO(b"abcd"), 8)

        self.assertNotIn(airdap_update.OTA_COMMIT, [item[0] for item in endpoint_out.writes])

    def test_selects_by_serial_and_reconnects_only_matching_device(self) -> None:
        first = object()
        second = object()
        serials = {first: "ADP-ONE", second: "ADP-TWO"}
        self.assertIs(
            airdap_update.select_airdap_device(
                [first, second], "ADP-TWO", serials.__getitem__
            ),
            second,
        )
        with self.assertRaisesRegex(airdap_update.UpdateError, "multiple"):
            airdap_update.select_airdap_device(
                [first, second], None, serials.__getitem__
            )

        discoveries = iter([[first], [first], [second]])
        monotonic = iter([0.0, 0.1, 0.2, 0.3])
        with mock.patch.object(
            airdap_update.time, "monotonic", side_effect=lambda: next(monotonic)
        ), mock.patch.object(airdap_update.time, "sleep"):
            reconnected = airdap_update.wait_for_reconnect(
                lambda: next(discoveries),
                "ADP-TWO",
                serials.__getitem__,
                timeout_seconds=1.0,
                poll_seconds=0.01,
            )
        self.assertIs(reconnected, second)

    def test_load_image_requires_a_regular_nonempty_file(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            empty = root / "empty.bin"
            empty.write_bytes(b"")
            valid = root / "airdap.bin"
            valid.write_bytes(b"abc")

            with self.assertRaisesRegex(airdap_update.UpdateError, "empty"):
                airdap_update.open_image(empty)
            with self.assertRaisesRegex(airdap_update.UpdateError, "regular"):
                airdap_update.open_image(root)
            stream, size = airdap_update.open_image(valid)
            try:
                self.assertEqual(size, 3)
                self.assertEqual(stream.read(), b"abc")
            finally:
                stream.close()


if __name__ == "__main__":
    unittest.main()
