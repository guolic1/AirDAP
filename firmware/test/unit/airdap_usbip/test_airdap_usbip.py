import asyncio
from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[3] / "tools"))
import airdap_usbip as bridge


class Backend:
    def __init__(self):
        self.calls = []
        self.rx = bytearray()
        self.closed = False

    async def connect(self):
        self.calls.append("connect")

    async def close(self):
        self.closed = True

    async def dap(self, data):
        self.calls.append(("dap", data))
        return b"\x00\x02\xfc\x01"

    async def configure(self, coding):
        self.calls.append(("configure", coding))

    async def write(self, data):
        self.calls.append(("write", data))

    async def read(self, capacity):
        data = bytes(self.rx[:capacity])
        del self.rx[:capacity]
        return data

    async def keepalive(self):
        self.calls.append("keepalive")


def submit(seq, ep, direction, size=0, data=b"", setup=bytes(8)):
    # Wire headers are independently encoded, including Linux non-ISO -1.
    return struct.pack(">IIIII Iiiii 8s", 1, seq, 0x10001, direction, ep,
                       0, size, 0, -1, 0, setup) + (data if not direction else b"")


class USBIPTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.backends = []
        def factory():
            backend = Backend()
            self.backends.append(backend)
            return backend
        self.bridge = bridge.Server("ADP-001122334455", factory)
        self.server = await asyncio.start_server(self.bridge.handle, "127.0.0.1", 0)
        self.port = self.server.sockets[0].getsockname()[1]
        self.connections = []

    async def asyncTearDown(self):
        for _, writer in self.connections:
            writer.close()
            await writer.wait_closed()
        self.server.close()
        await self.server.wait_closed()
        await self.bridge.close()

    async def open(self, attach=True):
        reader, writer = await asyncio.open_connection("127.0.0.1", self.port)
        self.connections.append((reader, writer))
        if attach:
            writer.write(bytes.fromhex("0111 8003 00000000") + b"1-1".ljust(32, b"\0"))
            self.assertEqual(await reader.readexactly(8), bytes.fromhex("0111 0003 00000000"))
            record = await reader.readexactly(312)
            self.assertEqual(record[256:288], b"1-1".ljust(32, b"\0"))
        return reader, writer

    async def reply(self, reader):
        header = await asyncio.wait_for(reader.readexactly(48), 2)
        command, seq, devid, direction, ep, status, size, _, packets, _ = struct.unpack(">IIIIIiiiii", header[:40])
        self.assertEqual((devid, direction, ep), (0, 0, 0))
        return command, seq, status, size, packets

    async def control(self, reader, writer, seq, request_type, request, value=0,
                      index=0, size=0, data=b""):
        setup = struct.pack("<BBHHH", request_type, request, value, index, size)
        writer.write(submit(seq, 0, request_type >> 7, size, data, setup))
        result = await self.reply(reader)
        response = await reader.readexactly(result[3]) if request_type & 0x80 else b""
        return result, response

    async def test_devlist_is_local_and_has_three_interfaces(self):
        reader, writer = await self.open(False)
        # Exercise fragmented TCP negotiation.
        for value in bytes.fromhex("0111 8005 00000000"):
            writer.write(bytes([value]))
            await writer.drain()
        result = await reader.read()
        self.assertEqual(result[:12], bytes.fromhex("0111 0005 00000000 00000001"))
        self.assertEqual(len(result), 336)
        self.assertEqual(result[-12:], bytes.fromhex("ff000000 02020100 0a000000"))
        self.assertEqual(self.backends, [])

    async def test_enumeration_and_windows_winusb_binding(self):
        r, w = await self.open()
        result, device = await self.control(r, w, 1, 0x80, 6, 0x100, size=18)
        self.assertEqual(result[2:4], (0, 18))
        self.assertEqual(device, bytes.fromhex("1201 1002 ef020140 3a30 2140 0201 01020301"))
        _, config = await self.control(r, w, 2, 0x80, 6, 0x200, size=255)
        self.assertEqual(len(config), int.from_bytes(config[2:4], "little"))
        self.assertEqual(config[4], 3)
        self.assertIn(bytes.fromhex("07050102400000 07058102400000"), config)
        _, bos = await self.control(r, w, 3, 0x80, 6, 0xF00, size=255)
        self.assertEqual(len(bos), 33)
        _, ms = await self.control(r, w, 4, 0xC0, 0x20, index=7, size=178)
        self.assertEqual(len(ms), 178)
        self.assertIn(b"WINUSB\0\0", ms)
        self.assertIn("DeviceInterfaceGUIDs\0".encode("utf-16le"), ms)
        result, _ = await self.control(r, w, 5, 0x80, 6, 0x600, size=10)
        self.assertEqual(result[2], -32)  # no high-speed qualifier

    async def test_dap_in_before_out_and_split_response(self):
        r, w = await self.open()
        w.write(submit(1, 1, 1, 2) + submit(2, 1, 0, 2, b"\0\xff"))
        replies = {}
        for _ in range(2):
            reply = await self.reply(r)
            replies[reply[1]] = reply
            if reply[1] == 1:
                self.assertEqual(await r.readexactly(2), b"\0\x02")
        self.assertEqual(replies[2][2:4], (0, 2))
        w.write(submit(3, 1, 1, 508))
        self.assertEqual((await self.reply(r))[2:4], (0, 2))
        self.assertEqual(await r.readexactly(2), b"\xfc\x01")
        self.assertIn(("dap", b"\0\xff"), self.backends[0].calls)

    async def test_cdc_line_coding_dtr_and_uart(self):
        r, w = await self.open()
        coding = struct.pack("<IBBB", 1000000, 2, 2, 7)
        self.assertEqual((await self.control(r, w, 1, 0x21, 0x20, index=1,
                                            size=7, data=coding))[0][2], 0)
        self.assertEqual(self.backends[0].calls, ["connect"])
        _, current = await self.control(r, w, 2, 0xA1, 0x21, index=1, size=7)
        self.assertEqual(current, coding)
        await self.control(r, w, 3, 0x21, 0x22, value=1, index=1)
        self.assertIn(("configure", (1000000, 2, 2, 7)), self.backends[0].calls)
        w.write(submit(4, 3, 0, 5, b"hello"))
        self.assertEqual((await self.reply(r))[2:4], (0, 5))
        self.assertIn(("write", b"hello"), self.backends[0].calls)
        self.backends[0].rx.extend(b"world")
        w.write(submit(5, 3, 1, 4096))
        self.assertEqual((await self.reply(r))[2:4], (0, 5))
        self.assertEqual(await r.readexactly(5), b"world")

    async def test_unlink_pending_input_does_not_consume_future_response(self):
        r, w = await self.open()
        w.write(submit(1, 1, 1, 508))
        w.write(struct.pack(">IIIIII24x", 2, 2, 0x10001, 0, 0, 1))
        self.assertEqual((await self.reply(r))[:3], (4, 2, -104))
        w.write(submit(3, 1, 0, 2, b"\0\xff"))
        self.assertEqual((await self.reply(r))[:3], (3, 3, 0))
        w.write(submit(4, 1, 1, 508))
        self.assertEqual((await self.reply(r))[:4], (3, 4, 0, 4))
        self.assertEqual(await r.readexactly(4), b"\0\x02\xfc\x01")

    async def test_invalid_configuration_stalls_without_changing_cache(self):
        r, w = await self.open()
        result, _ = await self.control(r, w, 1, 0x21, 0x20, index=1, size=7,
                                      data=bytes(7))
        self.assertEqual(result[2], -32)
        _, data = await self.control(r, w, 2, 0xA1, 0x21, index=1, size=7)
        self.assertEqual(data, struct.pack("<IBBB", 115200, 0, 0, 8))
        self.assertEqual(self.backends[0].calls, ["connect"])

    async def test_second_import_is_rejected(self):
        await self.open()
        r, w = await self.open(False)
        w.write(bytes.fromhex("0111 8003 00000000") + b"1-1".ljust(32, b"\0"))
        self.assertEqual(await r.readexactly(8), bytes.fromhex("0111 0003 00000001"))
        self.assertEqual(len(self.backends), 1)

    async def test_oversized_urb_disconnects_before_payload(self):
        r, w = await self.open()
        w.write(submit(1, 3, 0, 0x7fffffff))
        self.assertEqual(await asyncio.wait_for(r.read(), 2), b"")

    async def test_failure_unplugs_both_interfaces_and_allows_fresh_attach(self):
        r, w = await self.open()
        async def fail(data):
            raise bridge.uart.ProbeError("AirDAP response session or sequence mismatch")
        self.backends[0].dap = fail
        w.write(submit(1, 1, 1, 508) + submit(2, 1, 0, 2, b"\0\xff"))
        self.assertEqual(await asyncio.wait_for(r.read(), 2), b"")
        self.assertTrue(self.backends[0].closed)
        await self.open()
        self.assertEqual(len(self.backends), 2)

    async def test_inflight_output_unlink_unplugs_without_replaying(self):
        r, w = await self.open()
        started = asyncio.Event()
        async def wait(data):
            started.set()
            await asyncio.Event().wait()
        self.backends[0].dap = wait
        w.write(submit(1, 1, 0, 2, b"\0\xff"))
        await asyncio.wait_for(started.wait(), 2)
        w.write(struct.pack(">IIIIII24x", 2, 2, 0x10001, 0, 0, 1))
        self.assertEqual((await self.reply(r))[:3], (4, 2, -104))
        self.assertEqual(await asyncio.wait_for(r.read(), 2), b"")
        self.assertTrue(self.backends[0].closed)

    async def test_uart_unlink_during_poll_preserves_bytes(self):
        r, w = await self.open()
        started, finish = asyncio.Event(), asyncio.Event()
        async def read(capacity):
            started.set()
            await finish.wait()
            return b"abc"
        self.backends[0].read = read
        w.write(submit(1, 3, 1, 3))
        await asyncio.wait_for(started.wait(), 2)
        w.write(struct.pack(">IIIIII24x", 2, 2, 0x10001, 0, 0, 1))
        self.assertEqual((await self.reply(r))[:3], (4, 2, -104))
        finish.set()
        w.write(submit(3, 3, 1, 2))
        self.assertEqual((await self.reply(r))[:4], (3, 3, 0, 2))
        self.assertEqual(await r.readexactly(2), b"ab")
        w.write(submit(4, 3, 1, 3))
        self.assertEqual((await self.reply(r))[:4], (3, 4, 0, 1))
        self.assertEqual(await r.readexactly(1), b"c")

    async def test_dap_queue_preserves_pipelined_request_order(self):
        r, w = await self.open()
        for index in range(4):
            w.write(submit(index * 2 + 1, 1, 0, 2, bytes([0, index])))
            w.write(submit(index * 2 + 2, 1, 1, 508))
        for _ in range(8):
            reply = await self.reply(r)
            self.assertEqual(reply[2], 0)
            if reply[1] % 2 == 0:
                self.assertEqual(await r.readexactly(reply[3]), b"\0\x02\xfc\x01")
        self.assertEqual([call for call in self.backends[0].calls if call != "connect"],
                         [("dap", bytes([0, index])) for index in range(4)])

    async def test_closed_cdc_and_unsupported_break_stall(self):
        r, w = await self.open()
        w.write(submit(1, 3, 0, 1, b"x"))
        self.assertEqual((await self.reply(r))[2], -32)
        result, _ = await self.control(r, w, 2, 0x21, 0x23, index=1)
        self.assertEqual(result[2], -32)
        self.assertEqual(self.backends[0].calls, ["connect"])

    async def test_pending_limit_and_invalid_setup_fail_closed(self):
        r, w = await self.open()
        setup = struct.pack("<BBHHH", 0x80, 6, 0x100, 0, 18)
        w.write(submit(1, 0, 1, 8, setup=setup))
        self.assertEqual((await self.reply(r))[2], -32)
        for index in range(129):
            w.write(submit(index + 2, 1, 1, 508))
        self.assertEqual(await asyncio.wait_for(r.read(), 2), b"")

    async def test_keepalive_runs_while_dap_idle(self):
        r, w = await self.open()
        for _ in range(60):
            if "keepalive" in self.backends[0].calls:
                break
            await asyncio.sleep(0.05)
        self.assertIn("keepalive", self.backends[0].calls)


class BackendTest(unittest.IsolatedAsyncioTestCase):
    async def test_shared_token_ports_and_cdc_network_translation(self):
        clients = []
        class Client:
            def __init__(self, host, credential, **kwargs):
                self.args = (host, credential, kwargs)
                self.owner_session, self.token, self.firmware = 7, b"t" * 32, "445088b"
                self.calls = []
                self.connection = self
                clients.append(self)
            def shutdown(self, how):
                self.calls.append("shutdown")
            def close(self):
                self.calls.append("close")
            def request(self, kind, data, expected):
                self.calls.append((kind, data, expected))
                return b"\0\x02\xfc\x01" if kind == 3 else b""
            def acquire(self):
                self.calls.append("acquire")
            def configure(self, *coding):
                self.calls.append(coding)
            def write_all(self, data, timeout):
                self.calls.append(("write", data, timeout))
            def read(self, capacity):
                self.calls.append(("read", capacity))
                return b"data", 0
        backend = bridge.NetworkBackend("airdap.test", "credential")
        with patch.object(bridge.uart, "Client", Client):
            await backend.connect()
        self.assertEqual(clients[0].args[2], {"port": 3260, "timeout": 5.0})
        self.assertEqual(clients[1].args[2], {"port": 3261, "timeout": 5.0, "token": b"t" * 32})
        self.assertEqual(await backend.dap(b"\0\xff"), b"\0\x02\xfc\x01")
        await backend.configure((9600, 0, 0, 8))
        await backend.write(b"test")
        self.assertEqual(await backend.read(256), b"data")
        await backend.keepalive()
        self.assertEqual(clients[0].calls, [(3, b"\0\xff", 4), (7, b"", 7)])
        self.assertEqual(clients[1].calls, ["acquire", (9600, 0, 0, 8),
                         ("write", b"test", 5.0), ("read", 256), (7, b"", 7)])
        await backend.close()
        self.assertTrue(all(c.calls[-2:] == ["shutdown", "close"] for c in clients))

    async def test_uart_join_failure_closes_dap_session(self):
        from unittest.mock import MagicMock
        first = MagicMock()
        backend = bridge.NetworkBackend("airdap.test", "credential")
        with patch.object(bridge.uart, "Client", side_effect=[first, OSError("TLS failed")]):
            with self.assertRaises(OSError):
                await backend.connect()
        first.connection.shutdown.assert_called_once()
        first.close.assert_called_once()

    async def test_corrupt_dap_fails_and_uart_overflow_is_observable(self):
        from unittest.mock import MagicMock
        backend = bridge.NetworkBackend("airdap.test", "credential")
        client = MagicMock()
        backend.clients = [client, client]
        for response in (b"", bytes(509), b"\x01\0"):
            client.request.return_value = response
            with self.assertRaises(bridge.ProtocolError):
                await backend.dap(b"\0\xff")
        client.read.return_value = (b"x", 1)
        with self.assertLogs("airdap-usbip", level="WARNING") as messages:
            self.assertEqual(await backend.read(256), b"x")
        self.assertIn("1 bytes dropped", messages.output[0])
        with self.assertNoLogs("airdap-usbip", level="WARNING"):
            await backend.read(256)


if __name__ == "__main__":
    unittest.main()
