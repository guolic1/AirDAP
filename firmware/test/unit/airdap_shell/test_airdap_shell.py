from __future__ import annotations

import importlib.util
import sys
import threading
import time
import types
import unittest
from pathlib import Path
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[3] / "tools" / "airdap-shell.py"
SPEC = importlib.util.spec_from_file_location("airdap_shell", SCRIPT)
assert SPEC is not None and SPEC.loader is not None
airdap_shell = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = airdap_shell
SPEC.loader.exec_module(airdap_shell)


class FakeEndpoint:
    def __init__(self, address: int):
        self.bEndpointAddress = address
        self.bmAttributes = 2
        self.wMaxPacketSize = 64
        self.writes: list[bytes] = []
        self.reads: list[bytes] = []
        self.read_timeouts: list[int] = []

    def write(self, data: bytes, timeout: int) -> int:
        del timeout
        self.writes.append(bytes(data))
        return len(data)

    def read(self, size: int, timeout: int) -> bytes:
        del size
        self.read_timeouts.append(timeout)
        return self.reads.pop(0) if self.reads else b""


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
        if key != (airdap_shell.DEBUG_INTERFACE, 0):
            raise KeyError(key)
        return self.interface


class FakeDevice:
    iSerialNumber = 3

    def __init__(self, interface: FakeInterface):
        self.configuration = FakeConfiguration(interface)
        self.detached: list[int] = []
        self.attached: list[int] = []

    def get_active_configuration(self) -> FakeConfiguration:
        return self.configuration

    def is_kernel_driver_active(self, interface: int) -> bool:
        return interface == airdap_shell.DEBUG_INTERFACE

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


class FakeCommandTransport:
    def __init__(self, chunks: list[bytes]):
        self.chunks = chunks
        self.writes: list[bytes] = []
        self.read_timeouts: list[int | None] = []

    def write(self, data: bytes) -> None:
        self.writes.append(data)

    def read(self, timeout_ms: int | None = None) -> bytes:
        self.read_timeouts.append(timeout_ms)
        return self.chunks.pop(0) if self.chunks else b""


class ContinuousLogTransport(FakeCommandTransport):
    def __init__(self):
        super().__init__([])

    def read(self, timeout_ms: int | None = None) -> bytes:
        self.read_timeouts.append(timeout_ms)
        return b"log line\n"


class InteractiveTransport:
    timeout_ms = 2500

    def __init__(self):
        self.read_started = threading.Event()
        self.read_completed = threading.Event()
        self.read_timeouts: list[int | None] = []
        self.writes: list[bytes] = []

    def write(self, data: bytes) -> None:
        self.writes.append(data)

    def read(self, timeout_ms: int | None = None) -> bytes:
        self.read_timeouts.append(timeout_ms)
        self.read_started.set()
        time.sleep(0.02)
        self.read_completed.set()
        return b""


class NullStream:
    def isatty(self) -> bool:
        return False

    def write(self, data: bytes) -> int:
        return len(data)

    def flush(self) -> None:
        pass


class TtyStream(NullStream):
    def isatty(self) -> bool:
        return True

    def fileno(self) -> int:
        return 42


class AirDapShellTests(unittest.TestCase):
    def make_transport(self) -> tuple[object, FakeEndpoint, FakeEndpoint, FakeDevice, FakeUsbUtil]:
        endpoint_out = FakeEndpoint(airdap_shell.DEBUG_OUT_ENDPOINT)
        endpoint_in = FakeEndpoint(airdap_shell.DEBUG_IN_ENDPOINT)
        device = FakeDevice(FakeInterface([endpoint_out, endpoint_in]))
        usb_util = FakeUsbUtil()
        transport = airdap_shell.VendorShellTransport(device, usb_util, timeout_ms=250)
        return transport, endpoint_out, endpoint_in, device, usb_util

    def test_claims_only_debug_interface_and_frames_session(self) -> None:
        transport, endpoint_out, _, device, usb_util = self.make_transport()

        transport.open()
        transport.start_session()
        transport.close()

        self.assertEqual(device.detached, [airdap_shell.DEBUG_INTERFACE])
        self.assertEqual(usb_util.claimed, [(device, airdap_shell.DEBUG_INTERFACE)])
        self.assertEqual(endpoint_out.writes, [b"\x00", b"\x04"])
        self.assertEqual(usb_util.released, [(device, airdap_shell.DEBUG_INTERFACE)])
        self.assertEqual(device.attached, [airdap_shell.DEBUG_INTERFACE])
        self.assertEqual(usb_util.disposed, [device])

    def test_rejects_wrong_interface_endpoint_contract(self) -> None:
        wrong = FakeDevice(FakeInterface([FakeEndpoint(0x05), FakeEndpoint(0x85)]))

        with self.assertRaisesRegex(airdap_shell.ShellError, "endpoints"):
            airdap_shell.VendorShellTransport(wrong, FakeUsbUtil()).open()

    def test_selects_device_by_stable_usb_serial(self) -> None:
        first = object()
        second = object()
        serials = {first: "ADP-001122334455", second: "ADP-AABBCCDDEEFF"}

        selected = airdap_shell.select_airdap_device(
            [first, second],
            "ADP-AABBCCDDEEFF",
            serials.__getitem__,
        )

        self.assertIs(selected, second)
        with self.assertRaisesRegex(airdap_shell.ShellError, "multiple"):
            airdap_shell.select_airdap_device([first, second], None, serials.__getitem__)

    def test_command_mode_waits_for_next_prompt(self) -> None:
        transport = FakeCommandTransport([
            b"status\ntarget_mv=3300 ",
            b"usb_vbus_mv=5000\nairdap> ",
        ])

        transcript = airdap_shell.run_command(
            transport,
            "status",
            timeout_seconds=0.2,
        )

        self.assertEqual(transport.writes, [b"status\n"])
        self.assertIn(b"target_mv=3300", transcript)
        self.assertTrue(transcript.endswith(b"airdap> "))
        self.assertTrue(all(timeout <= 200 for timeout in transport.read_timeouts))

    def test_command_mode_ignores_log_redraw_prompt_before_command_echo(self) -> None:
        transport = FakeCommandTransport([
            b"background log\nairdap> ",
            b"status\ntarget_mv=3300\nairdap> ",
        ])

        transcript = airdap_shell.run_command(
            transport,
            "status",
            timeout_seconds=0.2,
        )

        self.assertIn(b"status\ntarget_mv=3300", transcript)
        self.assertEqual(len(transport.read_timeouts), 2)

    def test_command_timeout_applies_while_logs_keep_arriving(self) -> None:
        transport = ContinuousLogTransport()

        with mock.patch.object(
            airdap_shell.time,
            "monotonic",
            side_effect=[0.0, 0.05, 0.2],
        ):
            with self.assertRaisesRegex(airdap_shell.ShellError, "timed out"):
                airdap_shell.read_until_prompt(transport, timeout_seconds=0.1)

    def test_restart_completes_on_acknowledgement_without_prompt(self) -> None:
        transport = FakeCommandTransport([
            b"restart\nRestarting ",
            b"AirDAP...\n",
        ])

        transcript = airdap_shell.run_command(
            transport,
            "restart",
            timeout_seconds=0.2,
        )

        self.assertEqual(transport.writes, [b"restart\n"])
        self.assertTrue(transcript.endswith(b"Restarting AirDAP...\n"))

    def test_rejects_control_bytes_in_command_mode(self) -> None:
        transport = FakeCommandTransport([])

        with self.assertRaisesRegex(airdap_shell.ShellError, "printable ASCII"):
            airdap_shell.run_command(transport, "help\t", timeout_seconds=0.2)

    def test_open_does_not_change_global_usb_configuration(self) -> None:
        class FakeUsbError(Exception):
            pass

        class ErrorCore:
            USBError = FakeUsbError

        class UnconfiguredDevice:
            def __init__(self):
                self.set_calls = 0

            def get_active_configuration(self):
                raise FakeUsbError("not configured")

            def set_configuration(self) -> None:
                self.set_calls += 1

        device = UnconfiguredDevice()

        with self.assertRaisesRegex(airdap_shell.ShellError, "active USB configuration"):
            airdap_shell.VendorShellTransport(
                device,
                FakeUsbUtil(),
                usb_core=ErrorCore,
            ).open()
        self.assertEqual(device.set_calls, 0)

    def test_windows_navigation_keys_are_mapped_to_ansi_sequences(self) -> None:
        mappings = {
            b"H": b"\x1b[A",
            b"P": b"\x1b[B",
            b"K": b"\x1b[D",
            b"M": b"\x1b[C",
            b"G": b"\x1b[H",
            b"O": b"\x1b[F",
            b"S": b"\x1b[3~",
        }

        for windows_key, ansi_sequence in mappings.items():
            with self.subTest(windows_key=windows_key):
                keys = iter([b"\xe0", windows_key])
                self.assertEqual(
                    airdap_shell._read_windows_key(lambda: next(keys)),
                    ansi_sequence,
                )

    def test_unknown_windows_extended_key_is_consumed(self) -> None:
        keys = iter([b"\xe0", b"Q"])

        self.assertEqual(airdap_shell._read_windows_key(lambda: next(keys)), b"")

    def test_raw_terminal_keeps_lf_to_crlf_output_translation(self) -> None:
        saved_attributes = [0, 0, 0, 0, 0, 0, []]
        raw_attributes = [0, 0, 0, 0, 0, 0, []]
        fake_termios = types.SimpleNamespace(
            OPOST=0x01,
            ONLCR=0x02,
            TCSANOW=1,
            TCSADRAIN=2,
            tcgetattr=mock.Mock(
                side_effect=[saved_attributes.copy(), raw_attributes.copy()]
            ),
            tcsetattr=mock.Mock(),
        )
        fake_tty = types.SimpleNamespace(setraw=mock.Mock())

        with mock.patch.dict(
            sys.modules,
            {"termios": fake_termios, "tty": fake_tty},
        ):
            with airdap_shell.raw_terminal(TtyStream()):
                pass

        fake_tty.setraw.assert_called_once_with(42)
        configured = fake_termios.tcsetattr.call_args_list[0].args
        self.assertEqual(configured[0:2], (42, fake_termios.TCSANOW))
        self.assertEqual(
            configured[2][1] & (fake_termios.OPOST | fake_termios.ONLCR),
            fake_termios.OPOST | fake_termios.ONLCR,
        )
        fake_termios.tcsetattr.assert_called_with(
            42,
            fake_termios.TCSADRAIN,
            saved_attributes,
        )

    def test_interactive_ctrl_c_is_forwarded_and_reader_stops_before_return(self) -> None:
        transport = InteractiveTransport()
        keys: list[bytes | BaseException] = [KeyboardInterrupt(), b"\x1d"]

        def read_keyboard(_stream, _stop):
            transport.read_started.wait(0.2)
            value = keys.pop(0)
            if isinstance(value, BaseException):
                raise value
            return value

        with mock.patch.object(airdap_shell, "_read_keyboard", read_keyboard):
            airdap_shell.interactive_session(
                transport,
                NullStream(),
                NullStream(),
            )

        self.assertEqual(transport.writes, [b"\x03"])
        self.assertTrue(transport.read_timeouts)
        self.assertTrue(all(timeout <= 100 for timeout in transport.read_timeouts))
        self.assertTrue(transport.read_completed.is_set())

    def test_interactive_ctrl_d_sends_prefix_then_exits(self) -> None:
        transport = InteractiveTransport()
        keys = iter([b"status\x04ignored", b"\x1d"])

        with mock.patch.object(
            airdap_shell,
            "_read_keyboard",
            side_effect=lambda _stream, _stop: next(keys),
        ):
            airdap_shell.interactive_session(
                transport,
                NullStream(),
                NullStream(),
            )

        self.assertEqual(transport.writes, [b"status"])

    def test_interactive_nul_is_not_forwarded_as_session_start(self) -> None:
        transport = InteractiveTransport()
        keys = iter([b"\x00help", b"\x1d"])

        with mock.patch.object(
            airdap_shell,
            "_read_keyboard",
            side_effect=lambda _stream, _stop: next(keys),
        ):
            airdap_shell.interactive_session(
                transport,
                NullStream(),
                NullStream(),
            )

        self.assertEqual(transport.writes, [b"help"])

    def test_restart_must_be_last_in_command_sequence(self) -> None:
        with self.assertRaisesRegex(airdap_shell.ShellError, "last"):
            airdap_shell.validate_command_sequence(["restart", "status"])


if __name__ == "__main__":
    unittest.main()
