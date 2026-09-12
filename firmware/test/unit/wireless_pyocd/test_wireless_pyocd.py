import importlib.util
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[2] / 'hil' / 'wireless_pyocd.py'
spec = importlib.util.spec_from_file_location('wireless_pyocd', SCRIPT)
hil = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hil)


class FakeClient:
    def __init__(self, *args, **kwargs):
        self.firmware = 'test-version'
        self.closed = False
        self.calls = []
        self.response = b'\x00\x01\x01'

    def request(self, kind, payload, expected):
        self.calls.append((kind, payload, expected))
        return self.response

    def close(self):
        self.closed = True


class WirelessInterfaceTests(unittest.TestCase):
    def setUp(self):
        self.mock = patch.object(hil.uart, 'Client', FakeClient)
        self.mock.start()
        self.addCleanup(self.mock.stop)
        self.link = hil.AirDAPInterface('example.invalid',
            SimpleNamespace(device_id='ADP-001122334455'))
        self.link.open()
        self.addCleanup(self.link.close)

    def test_commands_use_authenticated_dap_frames(self):
        self.link.write([0, 0xF0])
        self.assertEqual(self.link.read(), [0, 1, 1])
        self.assertEqual(self.link.client.calls, [(3, b'\x00\xf0', 4)])

    def test_one_outstanding_request(self):
        self.link.write([0, 0xF0])
        with self.assertRaises(hil.VerificationError):
            self.link.write([0, 0xFF])
        self.link.read()
        with self.assertRaises(hil.VerificationError):
            self.link.read()

    def test_payload_boundaries(self):
        for data in (b'', bytes(509)):
            with self.assertRaises(hil.VerificationError):
                self.link.write(data)
        self.link.write(bytes(508))
        self.link.read()
        for size in (0, 509):
            with self.assertRaises(hil.VerificationError):
                self.link.set_packet_size(size)

    def test_bad_response_closes_connection(self):
        for response in (b'', b'\x02\x01', bytes(509)):
            with self.subTest(response_size=len(response)):
                self.link.open()
                client = self.link.client
                client.response = response
                with self.assertRaises(hil.VerificationError):
                    self.link.write([0, 0xF0])
                self.assertTrue(client.closed)
                self.assertIsNone(self.link.client)

    def test_transport_failure_does_not_retry_flash_command(self):
        client = self.link.client
        with patch.object(client, 'request', side_effect=OSError('connection lost')) as request:
            with self.assertRaises(OSError):
                self.link.write([5, 0, 1])
            self.assertEqual(request.call_count, 1)
        self.assertTrue(client.closed)

    def test_close_discards_pending_response_and_reopens(self):
        old = self.link.client
        self.link.write([0, 0xF0])
        self.link.close()
        self.link.open()
        self.assertTrue(old.closed)
        self.assertIsNot(self.link.client, old)
        with self.assertRaises(hil.VerificationError):
            self.link.read()

    def test_verify_reports_first_mismatch(self):
        with self.assertRaisesRegex(hil.VerificationError, '0x08000002'):
            hil.verify_bytes(0x08000000, b'abc', b'abd')
        with self.assertRaises(hil.VerificationError):
            hil.verify_bytes(0x08000000, b'abc', b'ab')


if __name__ == '__main__':
    unittest.main()
