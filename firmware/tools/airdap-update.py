#!/usr/bin/env python3
"""Development-only AirDAP application update over CMSIS-DAP USB Bulk."""

from __future__ import annotations

import argparse
import contextlib
import os
import stat
import struct
import sys
import time
from collections.abc import Callable, Iterable, Sequence
from dataclasses import dataclass
from pathlib import Path
from typing import Any, BinaryIO


USB_VID = 0x303A
USB_PID = 0x4021
DAP_INTERFACE = 0
DAP_OUT_ENDPOINT = 0x01
DAP_IN_ENDPOINT = 0x81
DAP_PACKET_SIZE = 508
OTA_CHUNK_SIZE = 496
DAP_STREAM_RECOVERY_SECONDS = 0.30
DAP_RECOVERY_RESPONSE_LIMIT = 4

DAP_DISCONNECT = 0x03
OTA_QUERY = 0x80
OTA_BEGIN = 0x81
OTA_WRITE = 0x82
OTA_COMMIT = 0x83
OTA_ABORT = 0x84
OTA_REBOOT = 0x85

OTA_PROTOCOL_VERSION = 1
OTA_FLAG_ROLLBACK = 1 << 0

OTA_STATUS_NAMES = {
    1: "invalid argument",
    2: "invalid state",
    3: "invalid image size",
    4: "invalid write offset",
    5: "incomplete image",
    6: "flash write error",
    7: "image validation error",
    8: "boot activation error",
    9: "internal error",
}


def _windows_usb_backend() -> Any | None:
    if os.name != "nt":
        return None
    try:
        import libusb_package
    except ImportError:
        return None
    return libusb_package.get_libusb1_backend()


class UpdateError(RuntimeError):
    """Raised when the update workflow cannot prove successful completion."""


@dataclass(frozen=True)
class OtaInfo:
    protocol_version: int
    flags: int
    max_image_size: int
    running_version: str


def _read_airdap_serial(device: Any, usb_util: Any) -> str | None:
    try:
        return usb_util.get_string(device, device.iSerialNumber)
    except Exception as error:
        if os.name == "nt":
            raise UpdateError(
                f"AirDAP {USB_VID:04X}:{USB_PID:04X} is not accessible from "
                f"Windows: {error}. If the device is attached to WSL, detach it "
                "from WSL before retrying and ensure USB interface 0 uses WinUSB"
            ) from error
        raise UpdateError(
            f"cannot read the USB serial from AirDAP {USB_VID:04X}:{USB_PID:04X}: "
            f"{error}"
        ) from error


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
        raise UpdateError(f"AirDAP {USB_VID:04X}:{USB_PID:04X}{suffix} was not found")
    if len(candidates) > 1:
        raise UpdateError("multiple AirDAP devices found; select one with --serial")
    return candidates[0]


def open_image(path: Path) -> tuple[BinaryIO, int]:
    if not path.is_file():
        raise UpdateError(f"image path is not a regular file: {path}")
    try:
        stream = path.open("rb")
    except OSError as error:
        raise UpdateError(f"cannot open image {path}: {error}") from error

    try:
        image_stat = os.fstat(stream.fileno())
        if not stat.S_ISREG(image_stat.st_mode):
            raise UpdateError(f"image path is not a regular file: {path}")
        if image_stat.st_size == 0:
            raise UpdateError(f"image is empty: {path}")
        return stream, image_stat.st_size
    except BaseException:
        stream.close()
        raise


class DapOtaTransport:
    def __init__(
        self,
        device: Any,
        usb_util: Any,
        usb_core: Any | None = None,
        timeout_ms: int = 2500,
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
            raise UpdateError(f"cannot read active USB configuration: {error}") from error

        try:
            interface = configuration[(DAP_INTERFACE, 0)]
        except (KeyError, IndexError) as error:
            raise UpdateError("AirDAP CMSIS-DAP interface 0 is absent") from error

        interface_class = (
            interface.bInterfaceClass,
            interface.bInterfaceSubClass,
            interface.bInterfaceProtocol,
        )
        if interface_class != (0xFF, 0x00, 0x00):
            raise UpdateError(
                f"DAP interface class is {interface_class!r}, expected (255, 0, 0)"
            )

        endpoints = list(interface)
        addresses = [endpoint.bEndpointAddress for endpoint in endpoints]
        if addresses != [DAP_OUT_ENDPOINT, DAP_IN_ENDPOINT]:
            raise UpdateError(
                f"DAP endpoints are {addresses!r}, expected "
                f"[{DAP_OUT_ENDPOINT:#04x}, {DAP_IN_ENDPOINT:#04x}]"
            )
        if any((endpoint.bmAttributes & 0x03) != 2 for endpoint in endpoints):
            raise UpdateError("DAP endpoints must use Bulk transfers")
        if any(endpoint.wMaxPacketSize != 64 for endpoint in endpoints):
            raise UpdateError("DAP endpoints must use 64-byte full-speed packets")

        try:
            if self.device.is_kernel_driver_active(DAP_INTERFACE):
                self.device.detach_kernel_driver(DAP_INTERFACE)
                self.detached_kernel_driver = True
        except Exception as error:
            if not isinstance(error, NotImplementedError) and not self._is_usb_error(error):
                raise UpdateError(f"cannot detach DAP-interface driver: {error}") from error

        try:
            self.usb_util.claim_interface(self.device, DAP_INTERFACE)
        except Exception as error:
            if self.detached_kernel_driver:
                with contextlib.suppress(Exception):
                    self.device.attach_kernel_driver(DAP_INTERFACE)
                self.detached_kernel_driver = False
            raise UpdateError(f"cannot claim AirDAP DAP interface: {error}") from error

        self.endpoint_out, self.endpoint_in = endpoints
        self.claimed = True

    def close(self) -> None:
        if self.claimed:
            with contextlib.suppress(Exception):
                self.usb_util.release_interface(self.device, DAP_INTERFACE)
            self.claimed = False
        if self.detached_kernel_driver:
            with contextlib.suppress(Exception):
                self.device.attach_kernel_driver(DAP_INTERFACE)
            self.detached_kernel_driver = False
        try:
            self.usb_util.dispose_resources(self.device)
        except Exception as error:
            if self._is_usb_error(error):
                return
            raise UpdateError(f"cannot dispose USB resources: {error}") from error

    def _write(self, request: bytes) -> None:
        if not self.claimed or self.endpoint_out is None:
            raise UpdateError("AirDAP DAP interface is not open")
        if not request or len(request) > DAP_PACKET_SIZE:
            raise UpdateError(f"invalid DAP request length {len(request)}")

        offset = 0
        try:
            while offset < len(request):
                written = int(
                    self.endpoint_out.write(
                        request[offset:],
                        timeout=self.timeout_ms,
                    )
                )
                if written <= 0:
                    raise UpdateError(
                        f"USB Bulk OUT stopped after {offset}/{len(request)} bytes"
                    )
                offset += written
        except UpdateError:
            raise
        except Exception as error:
            raise UpdateError(f"USB Bulk OUT failed: {error}") from error

    def _exchange(self, request: bytes) -> bytes:
        self._write(request)

        response = self._read_response()
        if response[0] != request[0]:
            raise UpdateError(
                f"command 0x{request[0]:02X} returned response "
                f"0x{response[0]:02X}"
            )
        return response

    def _read_response(self) -> bytes:
        if self.endpoint_in is None:
            raise UpdateError("AirDAP DAP interface is not open")
        try:
            response = bytes(
                self.endpoint_in.read(DAP_PACKET_SIZE, timeout=self.timeout_ms)
            )
        except Exception as error:
            raise UpdateError(f"USB Bulk IN failed: {error}") from error
        if not response:
            raise UpdateError("command returned an empty response")
        return response

    @staticmethod
    def _check_status(response: bytes, command: int, operation: str) -> None:
        if len(response) < 2:
            raise UpdateError(f"{operation} returned a short response")
        status_value = response[1]
        if status_value != 0:
            status_name = OTA_STATUS_NAMES.get(status_value, "unknown status")
            raise UpdateError(
                f"{operation} failed: {status_name} (status {status_value})"
            )
        if response[0] != command:
            raise UpdateError(f"{operation} returned the wrong command byte")

    def query(self) -> OtaInfo:
        response = self._exchange(bytes((OTA_QUERY,)))
        self._check_status(response, OTA_QUERY, "query")
        if len(response) < 9:
            raise UpdateError("query returned a short capability response")
        version_length = response[8]
        if len(response) != 9 + version_length:
            raise UpdateError("query returned an invalid version length")
        try:
            running_version = response[9:].decode("utf-8")
        except UnicodeDecodeError as error:
            raise UpdateError("query returned a non-UTF-8 running version") from error

        info = OtaInfo(
            protocol_version=response[2],
            flags=response[3],
            max_image_size=struct.unpack_from("<I", response, 4)[0],
            running_version=running_version,
        )
        if info.protocol_version != OTA_PROTOCOL_VERSION:
            raise UpdateError(
                f"unsupported OTA protocol version {info.protocol_version}"
            )
        if (info.flags & OTA_FLAG_ROLLBACK) == 0:
            raise UpdateError("device does not advertise OTA rollback support")
        return info

    def disconnect_debug(self) -> None:
        response = self._exchange(bytes((DAP_DISCONNECT,)))
        if response != bytes((DAP_DISCONNECT, 0)):
            raise UpdateError(f"DAP disconnect failed: {response.hex()}")

    def begin(self, image_size: int) -> None:
        request = bytes((OTA_BEGIN,)) + struct.pack("<I", image_size)
        response = self._exchange(request)
        if len(response) != 2:
            raise UpdateError("begin returned an invalid response length")
        self._check_status(response, OTA_BEGIN, "begin")

    def write(self, offset: int, data: bytes) -> int:
        if not data or len(data) > OTA_CHUNK_SIZE:
            raise UpdateError(f"invalid OTA write chunk length {len(data)}")
        request = bytes((OTA_WRITE,)) + struct.pack("<IH", offset, len(data)) + data
        response = self._exchange(request)
        self._check_status(response, OTA_WRITE, "write")
        if len(response) != 6:
            raise UpdateError("write returned an invalid response length")
        return struct.unpack_from("<I", response, 2)[0]

    def commit(self) -> None:
        response = self._exchange(bytes((OTA_COMMIT,)))
        if len(response) != 2:
            raise UpdateError("commit returned an invalid response length")
        self._check_status(response, OTA_COMMIT, "commit")

    def abort(self) -> None:
        self._write(bytes((OTA_ABORT,)))
        for _ in range(DAP_RECOVERY_RESPONSE_LIMIT):
            response = self._read_response()
            if response[0] != OTA_ABORT:
                continue
            if len(response) != 2:
                raise UpdateError("abort returned an invalid response length")
            self._check_status(response, OTA_ABORT, "abort")
            return
        raise UpdateError("abort response not received while resynchronizing")

    def reboot(self) -> None:
        # Firmware restarts inside this command and intentionally sends no response.
        self._write(bytes((OTA_REBOOT,)))


def upload_image(
    transport: DapOtaTransport,
    image: BinaryIO,
    image_size: int,
    progress: Callable[[int, int], None] | None = None,
) -> OtaInfo:
    if image_size <= 0:
        raise UpdateError("image is empty")

    before = transport.query()
    if image_size > before.max_image_size:
        raise UpdateError(
            f"image size {image_size} exceeds inactive slot capacity "
            f"{before.max_image_size}"
        )

    transport.disconnect_debug()
    begin_attempted = False
    committed = False
    try:
        begin_attempted = True
        transport.begin(image_size)
        offset = 0
        while offset < image_size:
            requested = min(OTA_CHUNK_SIZE, image_size - offset)
            chunk = image.read(requested)
            if not chunk:
                raise UpdateError(
                    f"image ended at {offset} bytes, expected {image_size}"
                )
            if len(chunk) > requested:
                raise UpdateError("image stream returned more bytes than requested")
            next_offset = transport.write(offset, chunk)
            expected_offset = offset + len(chunk)
            if next_offset != expected_offset:
                raise UpdateError(
                    f"device returned next offset {next_offset}, "
                    f"expected {expected_offset}"
                )
            offset = next_offset
            if progress is not None:
                progress(offset, image_size)

        if image.read(1):
            raise UpdateError("image contains data beyond its declared size")
        transport.commit()
        committed = True
    except BaseException:
        if begin_attempted and not committed:
            # A failed Bulk OUT may have left a partial variable-length frame
            # in firmware. Let its 250 ms stale-frame boundary expire before
            # sending ABORT so the new command cannot be consumed as payload.
            time.sleep(DAP_STREAM_RECOVERY_SECONDS)
            with contextlib.suppress(Exception):
                transport.abort()
        raise

    transport.reboot()
    return before


def wait_for_reconnect(
    find_devices: Callable[[], Iterable[Any]],
    serial: str,
    serial_getter: Callable[[Any], str | None],
    timeout_seconds: float,
    poll_seconds: float = 0.1,
) -> Any:
    deadline = time.monotonic() + timeout_seconds
    while True:
        matches = []
        for device in find_devices():
            try:
                if serial_getter(device) == serial:
                    matches.append(device)
            except Exception:
                continue
        if len(matches) == 1:
            return matches[0]
        if len(matches) > 1:
            raise UpdateError(f"multiple AirDAP devices report serial {serial}")
        if time.monotonic() >= deadline:
            raise UpdateError(
                f"timed out after {timeout_seconds:g}s waiting for {serial} to reconnect"
            )
        time.sleep(poll_seconds)


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Update a development AirDAP over its CMSIS-DAP USB interface",
    )
    parser.add_argument("image", type=Path, help="application image, normally build/airdap.bin")
    parser.add_argument("--serial", help="select one AirDAP by stable USB serial")
    parser.add_argument("--timeout-ms", type=int, default=2500, help="USB transfer timeout")
    parser.add_argument(
        "--reconnect-timeout",
        type=float,
        default=15.0,
        help="seconds to wait for the updated device to reconnect",
    )
    return parser


def _find_airdap_devices(usb_core: Any) -> list[Any]:
    return list(
        usb_core.find(
            find_all=True,
            backend=_windows_usb_backend(),
            idVendor=USB_VID,
            idProduct=USB_PID,
        )
        or []
    )


def _close_transport(
    transport: DapOtaTransport,
    operation_error: BaseException | None,
) -> None:
    if operation_error is None:
        transport.close()
    else:
        with contextlib.suppress(Exception):
            transport.close()


def main(argv: Sequence[str] | None = None) -> int:
    args = make_parser().parse_args(argv)
    if args.timeout_ms <= 0:
        raise UpdateError("--timeout-ms must be positive")
    if args.reconnect_timeout <= 0:
        raise UpdateError("--reconnect-timeout must be positive")

    try:
        import usb.core as usb_core
        import usb.util as usb_util
    except ImportError as error:
        raise UpdateError("PyUSB is required: python -m pip install pyusb") from error

    image, image_size = open_image(args.image)
    try:
        serial_getter = lambda candidate: _read_airdap_serial(  # noqa: E731
            candidate, usb_util
        )
        device = select_airdap_device(
            _find_airdap_devices(usb_core),
            args.serial,
            serial_getter,
        )
        serial = serial_getter(device)
        if not serial:
            raise UpdateError("selected AirDAP has no USB serial number")

        transport = DapOtaTransport(
            device,
            usb_util,
            usb_core=usb_core,
            timeout_ms=args.timeout_ms,
        )
        operation_error: BaseException | None = None
        try:
            transport.open()

            last_percent = -1

            def report_progress(written: int, total: int) -> None:
                nonlocal last_percent
                percent = written * 100 // total
                if percent != last_percent:
                    print(
                        f"\rUploading {written}/{total} bytes ({percent}%)",
                        end="",
                        flush=True,
                    )
                    last_percent = percent

            before = upload_image(
                transport,
                image,
                image_size,
                progress=report_progress,
            )
            print()
        except BaseException as error:
            operation_error = error
            raise
        finally:
            _close_transport(transport, operation_error)

        print(
            f"Committed {image_size} bytes to {serial}; "
            f"previous version: {before.running_version or '<unknown>'}"
        )

        reconnect_deadline = time.monotonic() + args.reconnect_timeout
        last_reconnect_error: BaseException | None = None
        while True:
            remaining = reconnect_deadline - time.monotonic()
            if remaining <= 0:
                detail = f": {last_reconnect_error}" if last_reconnect_error else ""
                raise UpdateError(
                    f"timed out waiting for {serial} to run the updated firmware{detail}"
                )
            reconnected = wait_for_reconnect(
                lambda: _find_airdap_devices(usb_core),
                serial,
                serial_getter,
                remaining,
            )
            verify_transport = DapOtaTransport(
                reconnected,
                usb_util,
                usb_core=usb_core,
                timeout_ms=args.timeout_ms,
            )
            verify_error: BaseException | None = None
            try:
                verify_transport.open()
                after = verify_transport.query()
                break
            except BaseException as error:
                verify_error = error
                last_reconnect_error = error
                time.sleep(0.1)
            finally:
                _close_transport(verify_transport, verify_error)

        print(
            f"AirDAP {serial} reconnected; running version: "
            f"{after.running_version or '<unknown>'}"
        )
        return 0
    finally:
        image.close()


def _run_cli(argv: Sequence[str] | None = None) -> int:
    try:
        return main(argv)
    except (UpdateError, OSError) as error:
        print(f"airdap-update: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(_run_cli())
