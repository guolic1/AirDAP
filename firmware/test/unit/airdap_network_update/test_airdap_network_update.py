import importlib.util
import io
from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import patch

tools = Path(__file__).resolve().parents[3] / "tools"
sys.path.insert(0, str(tools))
spec = importlib.util.spec_from_file_location("network_update", tools / "airdap-network-update.py")
update = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = update
spec.loader.exec_module(update)


class Transport:
    def __init__(self):
        self.calls = []
        self.bad_offset = False
        self.fail_commit = False

    def query(self):
        return update.OtaInfo(0x3f0000, 0x20000, 0x410000, True, "old")

    def begin(self, size): self.calls.append(("begin", size))
    def write(self, offset, data):
        self.calls.append(("write", offset, data))
        return offset + len(data) + int(self.bad_offset)
    def commit(self):
        self.calls.append(("commit",))
        if self.fail_commit: raise update.UpdateError("commit failed")
    def abort(self): self.calls.append(("abort",))
    def reboot(self): self.calls.append(("reboot",))


class NetworkUpdateTest(unittest.TestCase):
    def test_upload_sequential_commit(self):
        client = Transport()
        before = update.upload(client, io.BytesIO(b"abcdefgh"), 8, chunk_size=3)
        self.assertEqual(before.running_address, 0x20000)
        self.assertEqual(client.calls, [("begin", 8), ("write", 0, b"abc"),
            ("write", 3, b"def"), ("write", 6, b"gh"), ("commit",)])

    def test_bad_offset_and_short_image_cancel_without_commit(self):
        for bad in (True, False):
            client = Transport(); client.bad_offset = bad
            with self.assertRaises(update.UpdateError):
                update.upload(client, io.BytesIO(b"abc"), 8)
            self.assertEqual(client.calls[-1], ("abort",))
            self.assertNotIn(("commit",), client.calls)

    def test_failure_and_extra_image_cancel(self):
        for extra in (True, False):
            client = Transport(); client.fail_commit = not extra
            with self.assertRaises(update.UpdateError):
                update.upload(client, io.BytesIO(b"abcd"), 3 if extra else 4)
            self.assertEqual(client.calls[-1], ("abort",))

    def test_success_requires_switch_version_and_confirmation(self):
        before = Transport().query()
        for after in (before,
            update.OtaInfo(0x3f0000, 0x410000, 0x20000, False, "new"),
            update.OtaInfo(0x3f0000, 0x410000, 0x20000, True, "wrong")):
            with self.assertRaises(update.UpdateError): update.verify_updated(before, after, "new")
        update.verify_updated(before, update.OtaInfo(0x3f0000, 0x410000, 0x20000, True, "new"), "new")

    def test_control_status_lengths_and_query(self):
        client = update.Client.__new__(update.Client)
        client.firmware = "old"
        response = b"\x30\0\x01\x01" + struct.pack(">III?", 0x3f0000, 0x20000, 0x410000, True) + b"old"
        client.request = lambda *args: response
        self.assertEqual(client.query(), Transport().query())
        for response in (b"", b"\x31", b"\x32\0", b"\x31\x04", b"\x31\0x"):
            client.request = lambda *args: response
            with self.assertRaises(update.UpdateError): client.begin(8)

    def test_wire_golden_and_maximum_chunk(self):
        class Socket:
            def __init__(self, incoming): self.incoming, self.sent = incoming, b""
            def sendall(self, data): self.sent += data
            def recv(self, size):
                data, self.incoming = self.incoming[:min(3, size)], self.incoming[min(3, size):]
                return data
        client = update.Client.__new__(update.Client)
        client.frame_session, client.sequence = 0x01020304, 0
        client.connection = Socket(bytes.fromhex(
            "41444150 01 06 0000 01020304 00000001 0006 0000 32 00 00000003"))
        self.assertEqual(client.write(0, b"abc"), 3)
        self.assertEqual(client.connection.sent, bytes.fromhex(
            "41444150 01 05 0000 01020304 00000001 0008 0000 32 00000000 616263"))
        with self.assertRaises(update.UpdateError): client.write(0, bytes(update.MAX_CHUNK + 1))
        client.request = lambda kind, data, expected: bytes([data[0], 0]) + (4091).to_bytes(4, "big")
        self.assertEqual(client.write(0, bytes(update.MAX_CHUNK)), 4091)

    def test_image_descriptor(self):
        image = bytearray(288)
        image[0] = 0xe9
        struct.pack_into("<I", image, 32, 0xabcd5432)
        image[48:52] = b"new\0"
        self.assertEqual(update.image_version(io.BytesIO(image)), "new")
        image[32] = 0
        with self.assertRaises(update.UpdateError): update.image_version(io.BytesIO(image))


if __name__ == "__main__": unittest.main()
