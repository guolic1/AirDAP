import importlib.util
from pathlib import Path
import sys
import unittest

tools = Path(__file__).resolve().parents[3] / "tools"
sys.path.insert(0, str(tools))
spec = importlib.util.spec_from_file_location("uart_probe", tools / "airdap-uart-probe.py")
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


class Connection:
    def __init__(self, data):
        self.data = data
        self.sent = b""

    def recv(self, size):
        result, self.data = self.data[:min(size, 3)], self.data[min(size, 3):]
        return result

    def sendall(self, data):
        self.sent += data


def client(payload, kind=6):
    result = probe.Client.__new__(probe.Client)
    result.frame_session, result.sequence = 0x01020304, 0
    # Independent literal v1 header, not the production frame encoder.
    result.connection = Connection(bytes.fromhex(
        "41444150 01") + bytes([kind]) + bytes.fromhex(
        "0000 01020304 00000001") + len(payload).to_bytes(2, "big") + b"\0\0" + payload)
    return result


class UARTProbeTest(unittest.TestCase):
    def test_configure_golden_frame(self):
        value = client(b"\x12")
        value.configure(115200)
        self.assertEqual(value.connection.sent, bytes.fromhex(
            "41444150 01 05 0000 01020304 00000001 0008 0000 12 0001C200 00 00 08"))

    def test_status_and_read_golden(self):
        value = client(bytes.fromhex("10 000F4240 00 00 08 01 0100 00000123"))
        self.assertEqual(value.status(), dict(baud=1000000, stop=0, parity=0,
            bits=8, tx_owner=True, buffered=256, dropped=0x123))
        value = client(bytes.fromhex("14 00000123 A5 00 FF"))
        self.assertEqual(value.read(3), (b"\xA5\0\xFF", 0x123))

    def test_error_format(self):
        for code in (0x20, 0x21, 0x23):
            value = client(code.to_bytes(2, "big"), 8)
            probe.expect_error(value.acquire, code)

    def test_partial_and_zero_acceptance(self):
        for count in (0, 1, 3):
            value = client(b"\x13" + count.to_bytes(2, "big"))
            self.assertEqual(value.write(b"abc"), count)

    def test_invalid_responses_fail_closed(self):
        for payload in (b"", b"\x13", b"\x13\0\x04", b"\x14\0\x01"):
            with self.subTest(payload=payload), self.assertRaises(probe.ProbeError):
                client(payload).write(b"abc")
        with self.assertRaises(probe.ProbeError):
            client(b"\x14\0\0\0").read()
        with self.assertRaises(probe.ProbeError):
            client(b"\x14\0\0\0\0ab").read(1)
        value = client(b"\x11")
        value.frame_session = 5
        with self.assertRaises(probe.ProbeError):
            value.acquire()

    def test_bounds_before_io(self):
        for size in (0, 257):
            value = client(b"")
            with self.assertRaises(probe.ProbeError):
                value.write(bytes(size))
            with self.assertRaises(probe.ProbeError):
                value.read(size)
            self.assertEqual(value.connection.sent, b"")


if __name__ == "__main__":
    unittest.main()
