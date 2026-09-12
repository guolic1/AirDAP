#!/usr/bin/env python3
"""Program a target via pyOCD and authenticated AirDAP TCP, then read it back."""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import logging
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[2] / 'tools'
sys.path.insert(0, str(TOOLS))
from airdap_network_credential import load_credential

_spec = importlib.util.spec_from_file_location('airdap_uart_probe', TOOLS / 'airdap-uart-probe.py')
uart = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(uart)


class VerificationError(RuntimeError):
    pass


class AirDAPInterface:
    """pyOCD CMSIS-DAP interface with exactly one authenticated request in flight.

    No USB discovery or fallback is used. A failed exchange closes the session;
    retrying an uncertain target write would conceal a broken transport.
    """

    vid, pid = 0x303A, 0x4021
    vendor_name, product_name = 'AirDAP', 'Authenticated wireless CMSIS-DAP'
    is_bulk, has_swo_ep = True, False

    def __init__(self, host, credential, *, port=3260, timeout=10):
        self.host, self.credential = host, credential
        self.port, self.timeout = port, timeout
        self.client = None
        self.pending = None
        self.packet_size, self.packet_count = 508, 1
        self.firmware = None
        self.exchanges = 0

    def get_serial_number(self):
        return self.credential.device_id

    def get_packet_count(self):
        return 1

    def set_packet_count(self, count):
        if count != 1:
            raise VerificationError('AirDAP requires packet count 1')

    def get_packet_size(self):
        return self.packet_size

    def set_packet_size(self, size):
        if not 1 <= size <= 508:
            raise VerificationError('invalid AirDAP packet size')
        self.packet_size = size

    def open(self):
        if self.client is None:
            self.client = uart.Client(self.host, self.credential,
                port=self.port, timeout=self.timeout)
            self.firmware = self.client.firmware

    def close(self):
        client, self.client = self.client, None
        self.pending = None
        if client is not None:
            client.close()

    def write(self, data):
        if self.client is None or self.pending is not None:
            raise VerificationError('interface closed or response not consumed')
        request = bytes(data)
        if not 1 <= len(request) <= self.packet_size:
            raise VerificationError('DAP request exceeds negotiated packet size')
        try:
            response = self.client.request(3, request, 4)
            if not 1 <= len(response) <= self.packet_size or response[0] not in (request[0], 0xFF):
                raise VerificationError('invalid CMSIS-DAP response size or command')
            self.pending = list(response)
            self.exchanges += 1
        except BaseException:
            self.close()
            raise

    def read(self):
        if self.pending is None:
            raise VerificationError('no pending DAP response')
        response, self.pending = self.pending, None
        return response


def verify_bytes(address, expected, actual):
    if len(actual) != len(expected):
        raise VerificationError(f'short target read: {len(actual)}/{len(expected)}')
    for offset, (wanted, found) in enumerate(zip(expected, actual)):
        if wanted != found:
            raise VerificationError(f'readback mismatch at 0x{address + offset:08X}')


def make_session(interface, target, frequency):
    from pyocd.core.session import Session
    from pyocd.probe.cmsis_dap_probe import CMSISDAPProbe
    from pyocd.probe.pydapaccess.dap_access_cmsis_dap import DAPAccessCMSISDAP

    probe = CMSISDAPProbe(DAPAccessCMSISDAP(None, interface=interface))
    return Session(probe, options={
        'target_override': target, 'frequency': frequency, 'connect_mode': 'halt',
        'cmsis_dap.limit_packets': True, 'hide_programming_progress': True,
        'no_config': True, 'cache.enable_memory': False,
    })


def run(args, evidence):
    import pyocd
    from pyocd.flash.loader import FlashLoader

    credential = load_credential(args.credential)
    image = args.image.read_bytes()
    if not image:
        raise VerificationError('empty programming image')
    evidence.update(device_id=credential.device_id, host=args.host, port=args.port,
        transport='AirDAP TLS 1.3 PSK-DHE TCP; no USB transport',
        pyocd_version=pyocd.__version__, target=args.target,
        image=str(args.image.resolve()), image_size=len(image),
        image_sha256=hashlib.sha256(image).hexdigest(),
        base_address=f'0x{args.base_address:08X}', frequency_hz=args.frequency)
    interface = AirDAPInterface(args.host, credential, port=args.port)
    try:
        with make_session(interface, args.target, args.frequency) as session:
            target = session.target
            evidence['airdap_firmware'] = interface.firmware
            region = target.memory_map.get_region_for_address(args.base_address)
            if region is None or not region.is_flash or not region.contains_range(
                    start=args.base_address, length=len(image)):
                raise VerificationError('image does not fit selected target flash region')
            before = bytes(target.read_memory_block8(args.base_address, len(image)))
            if args.backup:
                # Exclusive creation prevents a rerun from destroying the original backup.
                with args.backup.open('xb') as backup:
                    backup.write(before)
            evidence['before_sha256'] = hashlib.sha256(before).hexdigest()
            started = time.monotonic()
            loader = FlashLoader(session, chip_erase='sector', smart_flash=False,
                                 trust_crc=False, keep_unwritten=True)
            loader.add_data(args.base_address, image)
            loader.commit()
            evidence['program_seconds'] = round(time.monotonic() - started, 3)
            target.halt()
            actual = bytes(target.read_memory_block8(args.base_address, len(image)))
            verify_bytes(args.base_address, image, actual)
            evidence['immediate_readback_sha256'] = hashlib.sha256(actual).hexdigest()
            target.reset()
    finally:
        interface.close()
        evidence['program_dap_exchanges'] = interface.exchanges

    # A new TLS and pyOCD session verifies target flash survived reset and reconnect.
    interface = AirDAPInterface(args.host, credential, port=args.port)
    try:
        with make_session(interface, args.target, args.frequency) as session:
            actual = bytes(session.target.read_memory_block8(args.base_address, len(image)))
            verify_bytes(args.base_address, image, actual)
            evidence['reconnect_readback_sha256'] = hashlib.sha256(actual).hexdigest()
            session.target.reset()
    finally:
        interface.close()
        evidence['verify_dap_exchanges'] = interface.exchanges


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('host')
    parser.add_argument('--credential', type=Path, required=True)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--target', required=True)
    parser.add_argument('--base-address', type=lambda s: int(s, 0), required=True)
    parser.add_argument('--frequency', type=int, default=500000)
    parser.add_argument('--port', type=int, default=3260)
    parser.add_argument('--backup', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args(argv)
    if args.frequency <= 0 or not 1 <= args.port <= 65535 or args.base_address < 0:
        parser.error('invalid frequency, port or base address')
    logging.basicConfig(level=logging.INFO, format='%(levelname)s %(message)s')
    evidence = {'status': 'running', 'started_utc': datetime.now(timezone.utc).isoformat()}
    try:
        run(args, evidence)
        evidence['status'] = 'passed'
    except Exception as error:
        evidence.update(status='failed', error=f'{type(error).__name__}: {error}')
        print(evidence['error'], file=sys.stderr)
    finally:
        evidence['finished_utc'] = datetime.now(timezone.utc).isoformat()
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(evidence, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(evidence, indent=2))
    return 0 if evidence['status'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
