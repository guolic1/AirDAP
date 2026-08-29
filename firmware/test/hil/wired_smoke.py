#!/usr/bin/env python3
"""Automated portions of the AirDAP wired firmware HIL acceptance."""

from __future__ import annotations

import argparse
import hashlib
import json
import platform
import re
import struct
import sys
import time
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Callable, Iterable, Sequence


USB_VID = 0x303A
USB_PID = 0x4021
USB_DAP_INTERFACE = 0
USB_CDC_CONTROL_INTERFACE = 1
USB_CDC_DATA_INTERFACE = 2
USB_DAP_OUT_ENDPOINT = 0x01
USB_DAP_IN_ENDPOINT = 0x81
USB_CDC_NOTIFICATION_ENDPOINT = 0x82
USB_CDC_OUT_ENDPOINT = 0x03
USB_CDC_IN_ENDPOINT = 0x83
USB_MAX_READ = 512
DAP_PACKET_SIZE = 508
DAP_MAX_BLOCK_READS = (DAP_PACKET_SIZE - 4) // 4
DAP_ACK_OK = 1

ID_DAP_INFO = 0x00
ID_DAP_CONNECT = 0x02
ID_DAP_DISCONNECT = 0x03
ID_DAP_TRANSFER_CONFIGURE = 0x04
ID_DAP_TRANSFER_BLOCK = 0x06
ID_DAP_RESET_TARGET = 0x0A
ID_DAP_SWJ_CLOCK = 0x11
ID_DAP_SWJ_SEQUENCE = 0x12
ID_DAP_SWD_CONFIGURE = 0x13

DAP_INFO_VENDOR = 0x01
DAP_INFO_PRODUCT = 0x02
DAP_INFO_SERIAL = 0x03
DAP_INFO_DAP_FIRMWARE = 0x04
DAP_INFO_PRODUCT_FIRMWARE = 0x09
DAP_INFO_CAPABILITIES = 0xF0
DAP_INFO_PACKET_COUNT = 0xFE
DAP_INFO_PACKET_SIZE = 0xFF

Exchange = Callable[[bytes], bytes]


class VerificationError(RuntimeError):
    """Raised when an observable HIL acceptance condition is not met."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise VerificationError(message)


def decode_dap_info(response: bytes, info_id: int) -> bytes:
    require(len(response) >= 2, f"DAP_Info 0x{info_id:02X} response is short")
    require(response[0] == ID_DAP_INFO, f"DAP_Info returned command 0x{response[0]:02X}")
    length = response[1]
    require(len(response) == length + 2, f"DAP_Info 0x{info_id:02X} length mismatch")
    return response[2:]


def exchange_info(exchange: Exchange, info_id: int) -> bytes:
    return decode_dap_info(exchange(bytes((ID_DAP_INFO, info_id))), info_id)


def decode_c_string(value: bytes, label: str) -> str:
    require(value.endswith(b"\0"), f"{label} is not NUL terminated")
    try:
        return value[:-1].decode("utf-8")
    except UnicodeDecodeError as error:
        raise VerificationError(f"{label} is not UTF-8") from error


def read_probe_information(exchange: Exchange) -> dict[str, Any]:
    vendor = decode_c_string(exchange_info(exchange, DAP_INFO_VENDOR), "DAP vendor")
    product = decode_c_string(exchange_info(exchange, DAP_INFO_PRODUCT), "DAP product")
    serial = decode_c_string(exchange_info(exchange, DAP_INFO_SERIAL), "DAP serial")
    dap_firmware = decode_c_string(
        exchange_info(exchange, DAP_INFO_DAP_FIRMWARE),
        "DAP firmware version",
    )
    product_firmware = decode_c_string(
        exchange_info(exchange, DAP_INFO_PRODUCT_FIRMWARE),
        "product firmware version",
    )
    capabilities = exchange_info(exchange, DAP_INFO_CAPABILITIES)
    packet_count = exchange_info(exchange, DAP_INFO_PACKET_COUNT)
    packet_size = exchange_info(exchange, DAP_INFO_PACKET_SIZE)

    require(vendor == "AirDAP", f"unexpected DAP vendor {vendor!r}")
    require("CMSIS-DAP" in product, f"unexpected DAP product {product!r}")
    require(re.fullmatch(r"ADP-[0-9A-F]{12}", serial) is not None,
            f"unexpected DAP serial {serial!r}")
    require(len(capabilities) == 2, "DAP capabilities must have two bytes")
    require((capabilities[0] & 0x01) != 0, "SWD capability is not advertised")
    require((capabilities[1] & 0x01) != 0, "USB COM capability is not advertised")
    require(packet_count == b"\x01", "DAP packet count must be one")
    require(packet_size == struct.pack("<H", DAP_PACKET_SIZE),
            f"DAP packet size must be {DAP_PACKET_SIZE}")

    return {
        "vendor": vendor,
        "product": product,
        "serial": serial,
        "dap_firmware": dap_firmware,
        "product_firmware": product_firmware,
        "capabilities": capabilities.hex(),
        "packet_count": packet_count[0],
        "packet_size": struct.unpack("<H", packet_size)[0],
    }


def expect_status(exchange: Exchange, request: bytes) -> None:
    response = exchange(request)
    require(response == bytes((request[0], 0x00)),
            f"command 0x{request[0]:02X} failed: {response.hex()}")


def configure_swd(exchange: Exchange, clock_hz: int) -> None:
    response = exchange(bytes((ID_DAP_CONNECT, 1)))
    require(response == bytes((ID_DAP_CONNECT, 1)),
            f"DAP_Connect SWD failed: {response.hex()}")
    expect_status(exchange, bytes((ID_DAP_TRANSFER_CONFIGURE, 0, 64, 0, 0, 0)))
    expect_status(exchange, bytes((ID_DAP_SWJ_CLOCK,)) + struct.pack("<I", clock_hz))
    expect_status(exchange, bytes((ID_DAP_SWD_CONFIGURE, 0)))

    # 64 high clocks, JTAG-to-SWD, 56 high clocks, then eight idle low clocks.
    sequence = (b"\xFF" * 8) + b"\x9E\xE7" + (b"\xFF" * 7) + b"\x00"
    require(len(sequence) * 8 == 144, "internal SWJ sequence length mismatch")
    expect_status(exchange, bytes((ID_DAP_SWJ_SEQUENCE, 144)) + sequence)


def decode_idcode_block(response: bytes, requested_count: int) -> list[int]:
    expected_length = 4 + requested_count * 4
    require(len(response) == expected_length,
            f"DAP_TransferBlock response is {len(response)}, expected {expected_length}")
    require(response[0] == ID_DAP_TRANSFER_BLOCK, "wrong TransferBlock response command")
    completed = struct.unpack_from("<H", response, 1)[0]
    require(completed == requested_count,
            f"TransferBlock completed {completed}/{requested_count} reads")
    require(response[3] == DAP_ACK_OK,
            f"TransferBlock status is 0x{response[3]:02X}, expected ACK OK")
    return list(struct.unpack_from(f"<{requested_count}I", response, 4))


def read_stable_idcode(
    exchange: Exchange,
    read_count: int,
) -> tuple[int, bool]:
    require(read_count >= DAP_MAX_BLOCK_READS,
            f"IDCODE read count must be at least {DAP_MAX_BLOCK_READS} to test 508-byte boundary")
    remaining = read_count
    values: list[int] = []
    checked_boundary = False

    while remaining:
        count = min(remaining, DAP_MAX_BLOCK_READS)
        request = bytes((ID_DAP_TRANSFER_BLOCK, 0, count & 0xFF, count >> 8, 0x02))
        response = exchange(request)
        block = decode_idcode_block(response, count)
        values.extend(block)
        remaining -= count

        if not checked_boundary:
            require(len(response) == DAP_PACKET_SIZE,
                    "first IDCODE block did not exercise the 508-byte response boundary")
            packet_size = exchange_info(exchange, DAP_INFO_PACKET_SIZE)
            require(packet_size == struct.pack("<H", DAP_PACKET_SIZE),
                    "command immediately after 508-byte response was corrupted")
            checked_boundary = True

    idcode = values[0]
    require(idcode not in (0, 0xFFFFFFFF), f"invalid DP IDCODE 0x{idcode:08X}")
    require((idcode & 1) == 1, f"DP IDCODE bit 0 is not set: 0x{idcode:08X}")
    require(all(value == idcode for value in values), "DP IDCODE is not stable")
    return idcode, checked_boundary


def collect_dap_evidence(
    exchange: Exchange,
    clock_hz: int,
    idcode_reads: int,
    exercise_reset: bool,
) -> dict[str, Any]:
    info = read_probe_information(exchange)
    connected = False
    try:
        configure_swd(exchange, clock_hz)
        connected = True
        idcode, boundary_checked = read_stable_idcode(exchange, idcode_reads)
        reset_checked = False
        if exercise_reset:
            response = exchange(bytes((ID_DAP_RESET_TARGET,)))
            require(response == bytes((ID_DAP_RESET_TARGET, 0, 0)),
                    f"DAP_ResetTarget failed: {response.hex()}")
            reset_checked = True
        return {
            "probe": info,
            "clock_hz": clock_hz,
            "idcode": f"0x{idcode:08X}",
            "idcode_reads": idcode_reads,
            "packet_boundary_508_checked": boundary_checked,
            "reset_command_checked": reset_checked,
        }
    finally:
        if connected:
            response = exchange(bytes((ID_DAP_DISCONNECT,)))
            require(response == bytes((ID_DAP_DISCONNECT, 0)),
                    f"DAP_Disconnect failed: {response.hex()}")


class PyUsbDapTransport:
    def __init__(self, device: Any, usb_core: Any, usb_util: Any, timeout_ms: int):
        self.device = device
        self.usb_core = usb_core
        self.usb_util = usb_util
        self.timeout_ms = timeout_ms
        self.configuration: Any = None
        self.interface: Any = None
        self.endpoint_out: Any = None
        self.endpoint_in: Any = None
        self.detached_kernel_driver = False
        self.claimed = False
        self.descriptor_evidence: dict[str, Any] = {}

    def open(self) -> None:
        try:
            self.configuration = self.device.get_active_configuration()
        except self.usb_core.USBError:
            self.device.set_configuration()
            self.configuration = self.device.get_active_configuration()

        interfaces = {
            (item.bInterfaceNumber, item.bAlternateSetting): item
            for item in self.configuration
        }
        require(set(interfaces) == {(0, 0), (1, 0), (2, 0)},
                f"unexpected USB interfaces {sorted(interfaces)}")
        dap = interfaces[(USB_DAP_INTERFACE, 0)]
        cdc_control = interfaces[(USB_CDC_CONTROL_INTERFACE, 0)]
        cdc_data = interfaces[(USB_CDC_DATA_INTERFACE, 0)]
        dap_endpoints = list(dap)
        cdc_control_endpoints = list(cdc_control)
        cdc_data_endpoints = list(cdc_data)

        require((self.device.bDeviceClass, self.device.bDeviceSubClass,
                 self.device.bDeviceProtocol) == (0xEF, 0x02, 0x01),
                "USB device class must be EF/02/01")
        require((dap.bInterfaceClass, dap.bInterfaceSubClass, dap.bInterfaceProtocol) ==
                (0xFF, 0x00, 0x00), "DAP interface class must be FF/00/00")
        require([endpoint.bEndpointAddress for endpoint in dap_endpoints] ==
                [USB_DAP_OUT_ENDPOINT, USB_DAP_IN_ENDPOINT],
                "DAP Bulk OUT/IN endpoint order is wrong")
        require([endpoint.bEndpointAddress for endpoint in cdc_control_endpoints] ==
                [USB_CDC_NOTIFICATION_ENDPOINT], "CDC notification endpoint is wrong")
        require([endpoint.bEndpointAddress for endpoint in cdc_data_endpoints] ==
                [USB_CDC_OUT_ENDPOINT, USB_CDC_IN_ENDPOINT], "CDC data endpoints are wrong")
        all_endpoints = dap_endpoints + cdc_control_endpoints + cdc_data_endpoints
        require([endpoint.wMaxPacketSize for endpoint in all_endpoints] ==
                [64, 64, 8, 64, 64], "USB endpoint packet sizes are wrong")
        require([(endpoint.bmAttributes & 0x03) for endpoint in all_endpoints] ==
                [2, 2, 3, 2, 2], "USB endpoint transfer types are wrong")

        interface_name = self.usb_util.get_string(self.device, dap.iInterface)
        require(interface_name is not None and "CMSIS-DAP" in interface_name,
                "DAP interface string does not contain CMSIS-DAP")
        serial = self.device.serial_number
        require(serial is not None and re.fullmatch(r"ADP-[0-9A-F]{12}", serial) is not None,
                f"unexpected USB serial {serial!r}")

        try:
            if self.device.is_kernel_driver_active(USB_DAP_INTERFACE):
                self.device.detach_kernel_driver(USB_DAP_INTERFACE)
                self.detached_kernel_driver = True
        except (NotImplementedError, self.usb_core.USBError):
            pass
        self.usb_util.claim_interface(self.device, USB_DAP_INTERFACE)
        self.claimed = True
        self.interface = dap
        self.endpoint_out, self.endpoint_in = dap_endpoints
        self.descriptor_evidence = {
            "vid": f"0x{self.device.idVendor:04X}",
            "pid": f"0x{self.device.idProduct:04X}",
            "serial": serial,
            "manufacturer": self.device.manufacturer,
            "product": self.device.product,
            "device_class": "EF/02/01",
            "dap_interface": "FF/00/00",
            "dap_endpoints": ["0x01", "0x81"],
            "cdc_endpoints": ["0x82", "0x03", "0x83"],
        }

    def exchange(self, request: bytes) -> bytes:
        require(self.claimed, "USB DAP interface is not open")
        written = self.endpoint_out.write(request, timeout=self.timeout_ms)
        require(written == len(request), f"USB DAP request truncated to {written}/{len(request)}")
        response = bytes(self.endpoint_in.read(USB_MAX_READ, timeout=self.timeout_ms))
        require(response, f"empty response for DAP command 0x{request[0]:02X}")
        require(response[0] == request[0],
                f"DAP response command 0x{response[0]:02X} != request 0x{request[0]:02X}")
        return response

    def close(self) -> None:
        if self.claimed:
            self.usb_util.release_interface(self.device, USB_DAP_INTERFACE)
            self.claimed = False
        if self.detached_kernel_driver:
            try:
                self.device.attach_kernel_driver(USB_DAP_INTERFACE)
            except (NotImplementedError, self.usb_core.USBError):
                pass
        self.usb_util.dispose_resources(self.device)


def open_usb_transport(serial_filter: str | None, timeout_ms: int) -> PyUsbDapTransport:
    try:
        import usb as usb_package
        import usb.core as usb_core
        import usb.util as usb_util
    except ImportError as error:
        raise VerificationError("PyUSB is required: python -m pip install pyusb") from error

    devices = list(usb_core.find(find_all=True, idVendor=USB_VID, idProduct=USB_PID) or [])
    if serial_filter is not None:
        devices = [device for device in devices if device.serial_number == serial_filter]
    require(devices, "no AirDAP USB device found")
    require(len(devices) == 1, "multiple AirDAP devices found; pass --serial")
    transport = PyUsbDapTransport(devices[0], usb_core, usb_util, timeout_ms)
    transport.open()
    transport.descriptor_evidence["pyusb_version"] = getattr(
        usb_package, "__version__", "unknown")
    return transport


def read_exact(link: Any, length: int, timeout_seconds: float) -> bytes:
    deadline = time.monotonic() + timeout_seconds
    data = bytearray()
    while len(data) < length and time.monotonic() < deadline:
        data.extend(link.read(length - len(data)))
    return bytes(data)


def run_uart_loopback(port: str, serial_module: Any | None = None) -> dict[str, Any]:
    if serial_module is None:
        try:
            import serial as serial_module
        except ImportError as error:
            raise VerificationError("pyserial is required: python -m pip install pyserial") from error

    cases = (
        (9600, serial_module.EIGHTBITS, serial_module.PARITY_NONE, serial_module.STOPBITS_ONE),
        (115200, serial_module.SEVENBITS, serial_module.PARITY_EVEN, serial_module.STOPBITS_ONE),
        (1000000, serial_module.EIGHTBITS, serial_module.PARITY_NONE, serial_module.STOPBITS_TWO),
        (115200, serial_module.FIVEBITS, serial_module.PARITY_ODD,
         serial_module.STOPBITS_ONE_POINT_FIVE),
    )
    results = []
    for index, (baud, data_bits, parity, stop_bits) in enumerate(cases):
        data_mask = (1 << data_bits) - 1
        payload = bytes(((index * 37 + offset) & data_mask) for offset in range(257))
        with serial_module.Serial(
            port=port,
            baudrate=baud,
            bytesize=data_bits,
            parity=parity,
            stopbits=stop_bits,
            timeout=0.1,
            write_timeout=2.0,
        ) as link:
            link.reset_input_buffer()
            link.reset_output_buffer()
            written = link.write(payload)
            require(written == len(payload), f"CDC write truncated at {baud} baud")
            link.flush()
            received = read_exact(link, len(payload), 3.0)
            require(received == payload, f"CDC loopback mismatch at {baud} baud")
        results.append({
            "baud": baud,
            "data_bits": data_bits,
            "parity": parity,
            "stop_bits": stop_bits,
            "bytes": len(payload),
        })
    return {
        "port": port,
        "pyserial_version": getattr(serial_module, "__version__", "unknown"),
        "cases": results,
    }


def compare_target_bytes(
    read_memory_block8: Callable[[int, int], Sequence[int]],
    base_address: int,
    expected: bytes,
    chunk_size: int = 1024,
) -> None:
    for offset in range(0, len(expected), chunk_size):
        wanted = expected[offset:offset + chunk_size]
        actual = bytes(read_memory_block8(base_address + offset, len(wanted)))
        if actual != wanted:
            mismatch = next(index for index, pair in enumerate(zip(actual, wanted))
                            if pair[0] != pair[1]) if len(actual) == len(wanted) else 0
            raise VerificationError(
                f"flash verify mismatch at 0x{base_address + offset + mismatch:08X}")


def parse_frequencies(value: str) -> list[int]:
    try:
        frequencies = [int(item, 0) for item in value.split(",") if item]
    except ValueError as error:
        raise argparse.ArgumentTypeError("frequencies must be comma-separated integers") from error
    if not frequencies or any(frequency <= 0 for frequency in frequencies):
        raise argparse.ArgumentTypeError("frequencies must be positive")
    return frequencies


def program_and_verify(
    serial: str,
    target_name: str,
    image_path: Path,
    base_address: int,
    frequencies: Iterable[int],
    reliability_cycles: int,
    cycle_frequency: int,
    erase_mode: str,
) -> dict[str, Any]:
    try:
        import pyocd
        from pyocd.core.helpers import ConnectHelper
        from pyocd.flash.file_programmer import FileProgrammer
    except ImportError as error:
        raise VerificationError("pyOCD is required: python -m pip install pyocd") from error

    image = image_path.read_bytes()
    require(image, "program image is empty")
    runs: list[dict[str, Any]] = []

    def run_once(frequency: int, phase: str, index: int) -> None:
        started = time.monotonic()
        session = ConnectHelper.session_with_chosen_probe(
            unique_id=serial,
            options={
                "frequency": frequency,
                "target_override": target_name,
                "connect_mode": "halt",
            },
        )
        require(session is not None, f"pyOCD did not find probe {serial}")
        with session:
            FileProgrammer(
                session,
                chip_erase=erase_mode,
                smart_flash=False,
                trust_crc=False,
            ).program(
                str(image_path),
                file_format="bin",
                base_address=base_address,
            )
            compare_target_bytes(session.target.read_memory_block8, base_address, image)
            session.target.reset()
        runs.append({
            "phase": phase,
            "index": index,
            "frequency_hz": frequency,
            "duration_seconds": round(time.monotonic() - started, 3),
        })

    for index, frequency in enumerate(frequencies, start=1):
        run_once(frequency, "frequency_sweep", index)
    for index in range(1, reliability_cycles + 1):
        run_once(cycle_frequency, "reliability", index)

    return {
        "target": target_name,
        "image": str(image_path.resolve()),
        "image_size": len(image),
        "image_sha256": hashlib.sha256(image).hexdigest(),
        "base_address": f"0x{base_address:08X}",
        "erase_mode": erase_mode,
        "pyocd_version": getattr(pyocd, "__version__", "unknown"),
        "reliability_cycles": reliability_cycles,
        "runs": runs,
    }


def make_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", help="AirDAP USB serial; required if multiple probes exist")
    parser.add_argument("--board-revision", help="populated AirDAP PCB revision for evidence")
    parser.add_argument("--module-ordering-code",
                        help="exact ESP32-S3 module ordering code for evidence")
    parser.add_argument("--reference-target", help="reference target MCU/board for evidence")
    parser.add_argument("--timeout-ms", type=int, default=3000, help="USB transfer timeout")
    parser.add_argument("--dap-clock", type=int, default=500000, help="raw SWD test clock")
    parser.add_argument("--idcode-reads", type=int, default=126,
                        help="stable DP IDCODE reads; minimum 126")
    parser.add_argument("--exercise-reset", action="store_true",
                        help="issue DAP_ResetTarget; capture the 1 ms pulse externally")
    parser.add_argument("--cdc-port", help="CDC port connected to a target-UART loopback fixture")
    parser.add_argument("--program-image", type=Path,
                        help="binary reference image to erase, program, and read back")
    parser.add_argument("--target", help="pyOCD target type for --program-image")
    parser.add_argument("--base-address", type=lambda value: int(value, 0),
                        help="flash address for the binary reference image")
    parser.add_argument("--frequencies", type=parse_frequencies,
                        default=parse_frequencies("500000,1000000,5000000"),
                        help="comma-separated pyOCD frequency sweep")
    parser.add_argument("--reliability-cycles", type=int, default=0,
                        help="additional reconnect/program/readback cycles; Stage 1 requires 100")
    parser.add_argument("--cycle-frequency", type=int, default=1000000,
                        help="frequency used for reliability cycles")
    parser.add_argument("--erase-mode", choices=("auto", "sector", "chip"), default="sector")
    parser.add_argument("--output", type=Path, help="write JSON evidence to this path")
    return parser


def write_evidence(evidence: dict[str, Any], output: Path | None) -> None:
    encoded = json.dumps(evidence, indent=2, sort_keys=True)
    if output is None:
        print(encoded)
        return
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(encoded + "\n", encoding="utf-8")
    print(f"wrote HIL evidence to {output}")


def validate_arguments(parser: argparse.ArgumentParser, args: argparse.Namespace) -> None:
    if args.timeout_ms <= 0 or args.dap_clock <= 0 or args.cycle_frequency <= 0:
        parser.error("timeouts and clocks must be positive")
    if args.idcode_reads < DAP_MAX_BLOCK_READS:
        parser.error(f"--idcode-reads must be at least {DAP_MAX_BLOCK_READS}")
    if args.reliability_cycles < 0:
        parser.error("--reliability-cycles cannot be negative")
    programming_fields = (args.program_image, args.target, args.base_address)
    if any(value is not None for value in programming_fields) and not all(
            value is not None for value in programming_fields):
        parser.error("--program-image, --target, and --base-address must be provided together")
    if args.program_image is not None and not args.program_image.is_file():
        parser.error(f"program image does not exist: {args.program_image}")


def main(argv: Sequence[str] | None = None) -> int:
    parser = make_parser()
    args = parser.parse_args(argv)
    validate_arguments(parser, args)
    evidence: dict[str, Any] = {
        "started_utc": datetime.now(timezone.utc).isoformat(),
        "host": platform.platform(),
        "hardware": {
            "airdap_board_revision": args.board_revision,
            "esp32_module_ordering_code": args.module_ordering_code,
            "reference_target": args.reference_target,
        },
        "status": "running",
    }

    transport: PyUsbDapTransport | None = None
    try:
        transport = open_usb_transport(args.serial, args.timeout_ms)
        evidence["usb"] = transport.descriptor_evidence
        evidence["dap"] = collect_dap_evidence(
            transport.exchange,
            args.dap_clock,
            args.idcode_reads,
            args.exercise_reset,
        )
        serial = evidence["dap"]["probe"]["serial"]
        require(serial == evidence["usb"]["serial"],
                "USB serial and DAP_Info serial do not match")
        transport.close()
        transport = None

        if args.cdc_port is not None:
            evidence["uart"] = run_uart_loopback(args.cdc_port)
        if args.program_image is not None:
            evidence["program_verify"] = program_and_verify(
                serial,
                args.target,
                args.program_image,
                args.base_address,
                args.frequencies,
                args.reliability_cycles,
                args.cycle_frequency,
                args.erase_mode,
            )
        evidence["status"] = "passed"
        evidence["finished_utc"] = datetime.now(timezone.utc).isoformat()
        write_evidence(evidence, args.output)
        return 0
    except Exception as error:
        evidence["status"] = "failed"
        evidence["error_type"] = type(error).__name__
        evidence["error"] = str(error)
        evidence["finished_utc"] = datetime.now(timezone.utc).isoformat()
        write_evidence(evidence, args.output)
        print(f"HIL verification failed: {error}", file=sys.stderr)
        return 1
    finally:
        if transport is not None:
            transport.close()


if __name__ == "__main__":
    raise SystemExit(main())
