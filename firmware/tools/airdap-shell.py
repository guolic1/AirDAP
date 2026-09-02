#!/usr/bin/env python3
"""Interactive host shell for the optional AirDAP Vendor Bulk interface."""

from __future__ import annotations

import argparse
import contextlib
import os
import select
import sys
import threading
import time
from collections.abc import Callable, Iterable, Iterator, Sequence
from typing import Any, BinaryIO, Protocol


USB_VID = 0x303A
USB_PID = 0x4021
DEBUG_INTERFACE = 3
DEBUG_OUT_ENDPOINT = 0x04
DEBUG_IN_ENDPOINT = 0x84
USB_READ_SIZE = 512
INTERACTIVE_READ_TIMEOUT_MS = 100
SESSION_START = b"\x00"
SESSION_START_COLOR = b"\x01"
SESSION_END = b"\x04"
PROMPT = b"airdap> "
RESTART_ACKNOWLEDGEMENT = b"Restarting AirDAP...\n"
ANSI_RESET = b"\x1b[0m"
ANSI_CYAN = b"\x1b[36m"
ANSI_YELLOW = b"\x1b[33m"
COLORED_PROMPT = ANSI_CYAN + PROMPT + ANSI_RESET
COLORED_RESTART_ACKNOWLEDGEMENT = (
    ANSI_YELLOW + RESTART_ACKNOWLEDGEMENT + ANSI_RESET
)
LOCAL_EOF = SESSION_END[0]  # Ctrl-D
LOCAL_EXIT = 0x1D  # Ctrl-]


def _windows_usb_backend() -> Any | None:
    if os.name != "nt":
        return None
    try:
        import libusb_package
    except ImportError:
        return None
    return libusb_package.get_libusb1_backend()


class ShellError(RuntimeError):
    """Raised for an observable host-tool or USB contract failure."""


class CommandTransport(Protocol):
    def write(self, data: bytes) -> None: ...
    def read(self, timeout_ms: int | None = None) -> bytes: ...


def select_airdap_device(
    devices: Iterable[Any],
    serial: str | None,
    serial_getter: Callable[[Any], str | None],
) -> Any:
    candidates = list(devices)
    if serial is not None:
        candidates = [device for device in candidates if serial_getter(device) == serial]
    if not candidates:
        suffix = f" with serial {serial}" if serial is not None else ""
        raise ShellError(f"AirDAP {USB_VID:04X}:{USB_PID:04X}{suffix} was not found")
    if len(candidates) > 1:
        raise ShellError("multiple AirDAP devices found; select one with --serial")
    return candidates[0]


class VendorShellTransport:
    def __init__(
        self,
        device: Any,
        usb_util: Any,
        usb_core: Any | None = None,
        timeout_ms: int = 100,
    ):
        self.device = device
        self.usb_util = usb_util
        self.usb_core = usb_core
        self.timeout_ms = timeout_ms
        self.endpoint_out: Any = None
        self.endpoint_in: Any = None
        self.detached_kernel_driver = False
        self.claimed = False

    def _is_usb_error(self, error: BaseException) -> bool:
        return self.usb_core is not None and isinstance(error, self.usb_core.USBError)

    def open(self) -> None:
        try:
            configuration = self.device.get_active_configuration()
        except Exception as error:
            raise ShellError(f"cannot read active USB configuration: {error}") from error

        try:
            interface = configuration[(DEBUG_INTERFACE, 0)]
        except (KeyError, IndexError) as error:
            raise ShellError(
                "AirDAP debug interface is absent; build firmware with "
                "sdkconfig.debug-shell.defaults"
            ) from error

        interface_class = (
            interface.bInterfaceClass,
            interface.bInterfaceSubClass,
            interface.bInterfaceProtocol,
        )
        if interface_class != (0xFF, 0x00, 0x00):
            raise ShellError(
                f"debug interface class is {interface_class!r}, expected (255, 0, 0)"
            )

        endpoints = list(interface)
        addresses = [endpoint.bEndpointAddress for endpoint in endpoints]
        if addresses != [DEBUG_OUT_ENDPOINT, DEBUG_IN_ENDPOINT]:
            raise ShellError(
                f"debug endpoints are {addresses!r}, expected "
                f"[{DEBUG_OUT_ENDPOINT:#04x}, {DEBUG_IN_ENDPOINT:#04x}]"
            )
        if any((endpoint.bmAttributes & 0x03) != 2 for endpoint in endpoints):
            raise ShellError("debug endpoints must use Bulk transfers")
        if any(endpoint.wMaxPacketSize != 64 for endpoint in endpoints):
            raise ShellError("debug endpoints must use 64-byte full-speed packets")

        try:
            if self.device.is_kernel_driver_active(DEBUG_INTERFACE):
                self.device.detach_kernel_driver(DEBUG_INTERFACE)
                self.detached_kernel_driver = True
        except Exception as error:
            if not isinstance(error, NotImplementedError) and not self._is_usb_error(error):
                raise ShellError(f"cannot detach debug-interface driver: {error}") from error

        try:
            self.usb_util.claim_interface(self.device, DEBUG_INTERFACE)
        except Exception as error:
            if self.detached_kernel_driver:
                with contextlib.suppress(Exception):
                    self.device.attach_kernel_driver(DEBUG_INTERFACE)
                self.detached_kernel_driver = False
            raise ShellError(f"cannot claim AirDAP debug interface: {error}") from error

        self.endpoint_out, self.endpoint_in = endpoints
        self.claimed = True

    def write(self, data: bytes) -> None:
        if not self.claimed or self.endpoint_out is None:
            raise ShellError("AirDAP debug interface is not open")
        offset = 0
        while offset < len(data):
            written = int(self.endpoint_out.write(data[offset:], timeout=self.timeout_ms))
            if written <= 0:
                raise ShellError(f"USB Bulk OUT stopped after {offset}/{len(data)} bytes")
            offset += written

    def read(self, timeout_ms: int | None = None) -> bytes:
        if not self.claimed or self.endpoint_in is None:
            raise ShellError("AirDAP debug interface is not open")
        try:
            effective_timeout = self.timeout_ms
            if timeout_ms is not None:
                effective_timeout = min(effective_timeout, timeout_ms)
            return bytes(
                self.endpoint_in.read(
                    USB_READ_SIZE,
                    timeout=effective_timeout,
                )
            )
        except Exception as error:
            timeout_type = getattr(self.usb_core, "USBTimeoutError", ())
            if timeout_type and isinstance(error, timeout_type):
                return b""
            raise ShellError(f"USB Bulk IN failed: {error}") from error

    def start_session(self, color: bool = False) -> None:
        self.write(SESSION_START_COLOR if color else SESSION_START)

    def close(self) -> None:
        if self.claimed:
            with contextlib.suppress(Exception):
                self.write(SESSION_END)
            with contextlib.suppress(Exception):
                self.usb_util.release_interface(self.device, DEBUG_INTERFACE)
            self.claimed = False
        if self.detached_kernel_driver:
            with contextlib.suppress(Exception):
                self.device.attach_kernel_driver(DEBUG_INTERFACE)
            self.detached_kernel_driver = False
        try:
            self.usb_util.dispose_resources(self.device)
        except Exception as error:
            raise ShellError(f"cannot dispose USB resources: {error}") from error


def read_until(
    transport: CommandTransport,
    marker: bytes,
    timeout_seconds: float,
    description: str,
) -> bytes:
    deadline = time.monotonic() + timeout_seconds
    received = bytearray()
    while marker not in received:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise ShellError(f"timed out waiting for {description}")
        chunk = transport.read(timeout_ms=max(1, int(remaining * 1000)))
        if chunk:
            received.extend(chunk)
            continue
        time.sleep(0.005)
    return bytes(received)


def read_until_prompt(
    transport: CommandTransport,
    timeout_seconds: float,
    marker: bytes = PROMPT,
) -> bytes:
    return read_until(
        transport,
        marker,
        timeout_seconds,
        "the AirDAP shell prompt",
    )


def read_until_command_prompt(
    transport: CommandTransport,
    encoded_command: bytes,
    timeout_seconds: float,
    prompt_marker: bytes = PROMPT,
) -> bytes:
    deadline = time.monotonic() + timeout_seconds
    echo_marker = encoded_command + b"\n"
    received = bytearray()
    echo_end: int | None = None

    while True:
        if echo_end is None:
            echo_index = received.find(echo_marker)
            if echo_index >= 0:
                echo_end = echo_index + len(echo_marker)
        if echo_end is not None and prompt_marker in received[echo_end:]:
            return bytes(received)

        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise ShellError("timed out waiting for the AirDAP shell prompt")
        chunk = transport.read(timeout_ms=max(1, int(remaining * 1000)))
        if chunk:
            received.extend(chunk)
        else:
            time.sleep(0.005)


def _encode_command(command: str) -> bytes:
    if not command or not command.strip(" "):
        raise ShellError("commands must be non-empty single-line text")
    try:
        encoded = command.encode("ascii")
    except UnicodeEncodeError as error:
        raise ShellError("commands must contain printable ASCII") from error
    if any(byte < 0x20 or byte > 0x7E for byte in encoded):
        raise ShellError("commands must contain printable ASCII")
    if len(encoded) >= 128:
        raise ShellError("command is too long for the 127-byte firmware input limit")
    return encoded


def _is_restart_command(command: str) -> bool:
    return [word for word in command.split(" ") if word] == ["restart"]


def _is_interactive_wifi_set(command: str) -> bool:
    return [word for word in command.split(" ") if word] == ["wifi", "set"]


def validate_command_sequence(commands: Sequence[str]) -> None:
    for index, command in enumerate(commands):
        _encode_command(command)
        if _is_interactive_wifi_set(command):
            raise ShellError("wifi set is interactive and cannot be used with -c")
        if _is_restart_command(command) and index != len(commands) - 1:
            raise ShellError("restart must be the last command in a -c sequence")


def run_command(
    transport: CommandTransport,
    command: str,
    timeout_seconds: float,
    color: bool = False,
) -> bytes:
    encoded = _encode_command(command)
    transport.write(encoded + b"\n")
    if _is_restart_command(command):
        return read_until(
            transport,
            COLORED_RESTART_ACKNOWLEDGEMENT if color else RESTART_ACKNOWLEDGEMENT,
            timeout_seconds,
            "the restart acknowledgement",
        )
    return read_until_command_prompt(
        transport,
        encoded,
        timeout_seconds,
        COLORED_PROMPT if color else PROMPT,
    )


def _color_enabled(
    mode: str,
    command_mode: bool,
    output_stream: BinaryIO,
) -> bool:
    if mode == "always":
        return True
    if mode == "never":
        return False
    if mode != "auto":
        raise ValueError(f"unsupported color mode: {mode}")
    return not command_mode and output_stream.isatty()


@contextlib.contextmanager
def raw_terminal(stream: BinaryIO) -> Iterator[None]:
    if os.name == "nt" or not stream.isatty():
        yield
        return

    import termios
    import tty

    descriptor = stream.fileno()
    saved = termios.tcgetattr(descriptor)
    try:
        tty.setraw(descriptor)
        raw = termios.tcgetattr(descriptor)
        raw[1] |= termios.OPOST | termios.ONLCR
        termios.tcsetattr(descriptor, termios.TCSANOW, raw)
        yield
    finally:
        termios.tcsetattr(descriptor, termios.TCSADRAIN, saved)


def _read_windows_key(getch: Callable[[], bytes]) -> bytes:
    data = getch()
    if data in (b"\x00", b"\xe0"):
        return {
            b"H": b"\x1b[A",
            b"P": b"\x1b[B",
            b"K": b"\x1b[D",
            b"M": b"\x1b[C",
            b"G": b"\x1b[H",
            b"O": b"\x1b[F",
            b"S": b"\x1b[3~",
        }.get(getch(), b"")
    return data


def _read_keyboard(stream: BinaryIO, stop: threading.Event) -> bytes:
    if os.name == "nt":
        import msvcrt

        while not stop.is_set():
            if msvcrt.kbhit():
                return _read_windows_key(msvcrt.getch)
            time.sleep(0.01)
        return b""

    ready, _, _ = select.select([stream], [], [], 0.1)
    if not ready:
        return b""
    return os.read(stream.fileno(), 256)


def interactive_session(
    transport: VendorShellTransport,
    input_stream: BinaryIO,
    output_stream: BinaryIO,
) -> None:
    stop = threading.Event()
    reader_error: list[BaseException] = []

    def read_usb() -> None:
        try:
            while not stop.is_set():
                data = transport.read(
                    timeout_ms=min(
                        transport.timeout_ms,
                        INTERACTIVE_READ_TIMEOUT_MS,
                    )
                )
                if data:
                    output_stream.write(data)
                    output_stream.flush()
                else:
                    time.sleep(0.005)
        except BaseException as error:
            reader_error.append(error)
            stop.set()

    reader = threading.Thread(target=read_usb, name="airdap-shell-rx")
    reader.start()
    try:
        with raw_terminal(input_stream):
            while not stop.is_set():
                try:
                    data = _read_keyboard(input_stream, stop)
                except KeyboardInterrupt:
                    transport.write(b"\x03")
                    continue
                if not data:
                    continue
                exit_index = next(
                    (
                        index
                        for index, byte in enumerate(data)
                        if byte in (LOCAL_EOF, LOCAL_EXIT)
                    ),
                    None,
                )
                if exit_index is not None:
                    data = data[:exit_index]
                data = data.replace(SESSION_START, b"")
                data = data.replace(SESSION_START_COLOR, b"")
                if data:
                    transport.write(data)
                if exit_index is not None:
                    break
    finally:
        stop.set()
        reader.join()
    if reader_error:
        raise ShellError(str(reader_error[0])) from reader_error[0]


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Open the optional AirDAP Vendor Bulk debug shell",
    )
    parser.add_argument("--serial", help="select one AirDAP by stable USB serial")
    parser.add_argument(
        "-c",
        "--command",
        action="append",
        default=[],
        help="run one command and exit; may be repeated",
    )
    parser.add_argument("--timeout-ms", type=int, default=100, help="USB transfer timeout")
    parser.add_argument(
        "--command-timeout",
        type=float,
        default=3.0,
        help="seconds to wait for each shell prompt",
    )
    parser.add_argument(
        "--color",
        choices=("auto", "always", "never"),
        default="auto",
        help="color output: auto for interactive TTYs, always, or never",
    )
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    args = make_parser().parse_args(argv)
    if args.timeout_ms <= 0:
        raise ShellError("--timeout-ms must be positive")
    if args.command_timeout <= 0:
        raise ShellError("--command-timeout must be positive")
    validate_command_sequence(args.command)

    try:
        import usb.core as usb_core
        import usb.util as usb_util
    except ImportError as error:
        raise ShellError("PyUSB is required: python -m pip install pyusb") from error

    devices = list(
        usb_core.find(
            find_all=True,
            backend=_windows_usb_backend(),
            idVendor=USB_VID,
            idProduct=USB_PID,
        )
        or []
    )
    device = select_airdap_device(
        devices,
        args.serial,
        lambda candidate: usb_util.get_string(candidate, candidate.iSerialNumber),
    )
    transport = VendorShellTransport(
        device,
        usb_util,
        usb_core=usb_core,
        timeout_ms=args.timeout_ms,
    )
    operation_error: BaseException | None = None
    try:
        transport.open()
        color = _color_enabled(
            args.color,
            command_mode=bool(args.command),
            output_stream=sys.stdout.buffer,
        )
        transport.start_session(color=color)
        if args.command:
            sys.stdout.buffer.write(
                read_until_prompt(
                    transport,
                    args.command_timeout,
                    COLORED_PROMPT if color else PROMPT,
                )
            )
            for command in args.command:
                sys.stdout.buffer.write(
                    run_command(
                        transport,
                        command,
                        args.command_timeout,
                        color=color,
                    )
                )
            sys.stdout.buffer.flush()
        else:
            interactive_session(transport, sys.stdin.buffer, sys.stdout.buffer)
        return 0
    except BaseException as error:
        operation_error = error
        raise
    finally:
        if operation_error is None:
            transport.close()
        else:
            with contextlib.suppress(Exception):
                transport.close()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ShellError, OSError) as error:
        print(f"airdap-shell: {error}", file=sys.stderr)
        raise SystemExit(1) from error
