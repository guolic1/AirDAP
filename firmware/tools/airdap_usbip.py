"""Local USB/IP 1.1.1 device backed by authenticated AirDAP TCP services.

Wire reference: https://www.kernel.org/doc/html/latest/usb/usbip_protocol.html
Only the local virtual host controller is a USB/IP peer. The network side
always uses the existing TLS-PSK client and validates AirDAP frame sequences.
"""

from __future__ import annotations

import asyncio
from collections import deque
import importlib.util
import logging
from pathlib import Path
import socket
import struct


_spec = importlib.util.spec_from_file_location(
    "airdap_usbip_uart", Path(__file__).with_name("airdap-uart-probe.py"))
uart = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(uart)

LOG = logging.getLogger("airdap-usbip")
BUSID = b"1-1".ljust(32, b"\0")
DEVID = 0x10001
VERSION = 0x0111
MAX_TRANSFER = 65536
MAX_PENDING = 128
DAP_PACKET_SIZE = 508
DAP_PACKET_COUNT = 4
KEEPALIVE_INTERVAL = 0.5
KEEPALIVE_TIMEOUT = 1.5
# USB/IP status values are Linux errno values, including on Windows.
STALL = -32
NO_MEMORY = -12
CONNECTION_RESET = -104


class ProtocolError(RuntimeError):
    """Invalid USB/IP input or a broken upstream contract."""


class Stall(Exception):
    """A USB request is unsupported or invalid; stall its control transfer."""


class NetworkBackend:
    def __init__(self, host, credential, dap_port=3260, uart_port=3261, timeout=5.0):
        self.host = host
        self.credential = credential
        self.ports = (dap_port, uart_port)
        self.timeout = timeout
        self.clients = []
        self.locks = (asyncio.Lock(), asyncio.Lock())
        self.uart_dropped = 0

    def _connect(self):
        try:
            self.clients.append(uart.Client(self.host, self.credential,
                port=self.ports[0], timeout=self.timeout))
            self.clients.append(uart.Client(self.host, self.credential,
                port=self.ports[1], token=self.clients[0].token, timeout=self.timeout))
            if (self.clients[0].owner_session != self.clients[1].owner_session or
                    self.clients[0].token != self.clients[1].token or
                    self.clients[0].firmware != self.clients[1].firmware):
                raise ProtocolError("DAP and UART did not join the same device session")
        except BaseException:
            self._close()
            raise

    async def connect(self):
        task = asyncio.create_task(asyncio.to_thread(self._connect))
        try:
            await asyncio.shield(task)
        except asyncio.CancelledError:
            # A cancelled attach must not orphan sockets created by its thread.
            await asyncio.gather(task, return_exceptions=True)
            self._close()
            raise

    def _close(self):
        for client in self.clients:
            try:
                client.connection.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass  # An already disconnected socket still needs close().
            client.close()

    async def close(self):
        self._close()

    async def _call(self, channel, operation):
        async with self.locks[channel]:
            return await asyncio.to_thread(operation, self.clients[channel])

    async def dap(self, data):
        if not 1 <= len(data) <= DAP_PACKET_SIZE:
            raise ProtocolError("CMSIS-DAP request exceeds the firmware packet limit")
        response = await self._call(0, lambda c: c.request(3, data, 4))
        if (not 1 <= len(response) <= DAP_PACKET_SIZE or
                response[0] not in (data[0], 0xFF)):
            raise ProtocolError("invalid CMSIS-DAP response length or command")
        return response

    async def configure(self, coding):
        def apply(client):
            client.acquire()
            client.configure(*coding)
        await self._call(1, apply)

    async def write(self, data):
        await self._call(1, lambda c: c.write_all(data, timeout=self.timeout))

    async def read(self, capacity):
        data, dropped = await self._call(1, lambda c: c.read(capacity))
        if dropped != self.uart_dropped:
            # The firmware subscriber exists even before a COM application opens.
            # A full RX ring must not disconnect an unrelated DAP programming job.
            LOG.warning("Target UART RX overflow: %d bytes dropped in this session", dropped)
            self.uart_dropped = dropped
        return data

    async def keepalive(self):
        def ping(client):
            previous = client.connection.gettimeout()
            client.connection.settimeout(min(previous, KEEPALIVE_TIMEOUT) if previous is not None else KEEPALIVE_TIMEOUT)
            try:
                return client.request(7, b"", 7)
            finally:
                client.connection.settimeout(previous)
        for index in range(2):
            # Active traffic has its own deadline. Never queue an idle UART probe
            # behind a long DAP command, or alter that command's socket timeout.
            if self.locks[index].locked():
                continue
            response = await self._call(index, ping)
            if response:
                raise ProtocolError("AirDAP KEEPALIVE response must be empty")


class Descriptors:
    """The firmware's base CMSIS-DAP/CDC layout, without the USB-only shell."""

    def __init__(self, device_id):
        self.device = bytes.fromhex("1201 1002 ef020140 3a30 2140 0201 01020301")
        body = bytes.fromhex(
            "0904 0000 02 ff0000 04"       # DAP interface 0
            "0705 01 02 4000 00"          # Bulk OUT must precede IN
            "0705 81 02 4000 00"
            "080b 01 02 020200 05"        # CDC IAD
            "0904 0100 01 020201 05"       # CDC control interface 1
            "0524 00 2001"                # CDC 1.20 header
            "0524 01 00 02"               # call management, interface 2
            "0424 02 02"                  # line coding/control state, no break
            "0524 06 01 02"               # union
            "0705 82 03 0800 10"          # serial state notification
            "0904 0200 02 0a0000 00"       # CDC data interface 2
            "0705 03 02 4000 00"
            "0705 83 02 4000 00")
        self.configuration = struct.pack("<BBHBBBBB", 9, 2, 9 + len(body),
                                         3, 1, 0, 0x80, 50) + body
        name = "DeviceInterfaceGUIDs\0".encode("utf-16le")
        guid = "{E00ECB98-DD2B-4E70-8471-A7223FADDAF9}\0\0".encode("utf-16le")
        prop = struct.pack("<HHHH", 132, 4, 7, len(name)) + name
        prop += struct.pack("<H", len(guid)) + guid
        self.ms_os = (struct.pack("<HHIH", 10, 0, 0x06030000, 178) +
            struct.pack("<HHBBH", 8, 1, 0, 0, 168) +
            struct.pack("<HHBBH", 8, 2, 0, 0, 160) +
            struct.pack("<HH8s8s", 20, 3, b"WINUSB", b"") + prop)
        self.bos = (bytes.fromhex("050f210001 1c100500") +
            bytes.fromhex("df60ddd88945c74c9cd2659d9e648a9f") +
            struct.pack("<IHBB", 0x06030000, len(self.ms_os), 0x20, 0))
        self.strings = [b"\x04\x03\x09\x04"]
        for value in ("AirDAP", "AirDAP CMSIS-DAP v2 (Network)",
                      device_id + "-NET", "CMSIS-DAP v2", "AirDAP Target UART"):
            text = value.encode("utf-16le")
            self.strings.append(bytes([len(text) + 2, 3]) + text)

    def get(self, value, language):
        kind, index = value >> 8, value & 255
        if kind == 3 and index < len(self.strings) and language in (0, 0x409):
            return self.strings[index]
        if index == 0 and language == 0:
            descriptor = {1: self.device, 2: self.configuration, 15: self.bos}.get(kind)
            if descriptor is not None:
                return descriptor
        raise Stall()

    def record(self):
        return struct.pack(">256s32sIIIHHHBBBBBB", b"/airdap/1-1", BUSID,
                           1, 1, 2, 0x303A, 0x4021, 0x0102, 0xEF, 2, 1, 1, 1, 3)


class URB:
    def __init__(self, header):
        (self.command, self.seq, self.devid, self.direction, self.ep,
         self.flags, self.size, self.start, self.packets, self.interval,
         self.setup) = struct.unpack(">IIIIIIiiii8s", header)
        self.data = b""
        self.active = False


class Session:
    def __init__(self, reader, writer, descriptors, backend):
        self.reader, self.writer = reader, writer
        self.descriptors, self.backend = descriptors, backend
        self.pending = {}
        self.outputs = {0: asyncio.Queue(32), 1: asyncio.Queue(DAP_PACKET_COUNT),
                        3: asyncio.Queue(32)}
        self.responses = deque()
        self.response_space = asyncio.Event()
        self.response_space.set()
        self.coding = (115200, 0, 0, 8)
        self.line_state = 0
        self.configuration = 0
        self.notification = b""

    def send(self, packet):
        if self.writer.is_closing():
            raise ConnectionError("USB/IP client disconnected")
        if self.writer.transport.get_write_buffer_size() > 2 * MAX_TRANSFER:
            raise ProtocolError("USB/IP client stopped receiving completions")
        self.writer.write(packet)

    def complete(self, urb, data=b"", status=0):
        if self.pending.get(urb.seq) is not urb:
            return  # Successfully unlinked requests receive no RET_SUBMIT.
        del self.pending[urb.seq]
        size = 0 if status else (len(data) if urb.direction else urb.size)
        self.send(struct.pack(">IIIIIiiiii8x", 3, urb.seq, 0, 0, 0, status,
                              size, 0, 0, 0) + (data if urb.direction and not status else b""))

    def first_input(self, ep):
        return next((urb for urb in self.pending.values()
                     if urb.ep == ep and urb.direction), None)

    def flush_dap(self):
        while self.responses and (urb := self.first_input(1)) is not None:
            response = self.responses.popleft()
            self.complete(urb, response[:urb.size])
            if len(response) > urb.size:
                self.responses.appendleft(response[urb.size:])
        if len(self.responses) < DAP_PACKET_COUNT:
            self.response_space.set()

    def flush_notification(self):
        if self.notification and (urb := self.first_input(2)) is not None:
            data, self.notification = self.notification[:urb.size], self.notification[urb.size:]
            self.complete(urb, data)

    async def receive(self):
        while True:
            try:
                header = await self.reader.readexactly(48)
            except asyncio.IncompleteReadError as error:
                if error.partial:
                    raise ProtocolError("truncated USB/IP header") from error
                return
            urb = URB(header)
            if urb.devid != DEVID or urb.direction not in (0, 1):
                raise ProtocolError("invalid USB/IP device or direction")
            if urb.command == 2:
                if urb.ep != 0:
                    raise ProtocolError("invalid UNLINK endpoint")
                target = self.pending.pop(urb.flags, None)
                status = CONNECTION_RESET if target else 0
                self.send(struct.pack(">IIIIIi24x", 4, urb.seq, 0, 0, 0, status))
                if target is not None and target.active:
                    raise ProtocolError("in-flight output cancelled; detach to discard uncertain results")
                continue
            if (urb.command != 1 or not 0 <= urb.size <= MAX_TRANSFER or
                    urb.packets not in (0, -1) or urb.seq in self.pending):
                raise ProtocolError("invalid, duplicate, oversized or isochronous URB")
            if len(self.pending) >= MAX_PENDING:
                raise ProtocolError("too many pending USB/IP requests")
            if not urb.direction:
                urb.data = await asyncio.wait_for(self.reader.readexactly(urb.size), 5)
            self.pending[urb.seq] = urb
            if urb.ep == 0 or (urb.ep in (1, 3) and not urb.direction):
                try:
                    self.outputs[urb.ep].put_nowait(urb)
                except asyncio.QueueFull:
                    self.complete(urb, status=NO_MEMORY)
            elif urb.ep in (1, 2, 3) and urb.direction:
                if urb.size == 0:
                    self.complete(urb)
                else:
                    self.flush_dap()
                    self.flush_notification()
            else:
                self.complete(urb, status=STALL)
            await self.writer.drain()

    async def control(self, urb):
        request_type, request, value, index, size = struct.unpack("<BBHHH", urb.setup)
        if (urb.direction != request_type >> 7 or size != urb.size or
                (not urb.direction and size != len(urb.data))):
            raise Stall()
        if request_type == 0x80 and request == 6:
            return self.descriptors.get(value, index)[:size]
        if (request_type, request, value, index) == (0xC0, 0x20, 0, 7):
            return self.descriptors.ms_os[:size]
        if (request_type, request, value, index, size) == (0x80, 8, 0, 0, 1):
            return bytes([self.configuration])
        if request_type == 0 and request == 9 and index == size == 0 and value in (0, 1):
            if value == 0 and self.configuration:
                # Closing either authenticated channel revokes both. Re-enumerate
                # rather than preserving ownership across a USB deconfiguration.
                raise ProtocolError("USB deconfigured; detach and attach to reopen")
            self.configuration = value
            return b""
        if request_type == 0 and request == 5 and index == size == 0 and value <= 127:
            return b""  # The virtual controller owns the USB address.
        if request_type in (0x80, 0x81, 0x82) and request == 0 and value == 0 and size == 2:
            valid = ((request_type == 0x80 and index == 0) or
                     (request_type == 0x81 and index in (0, 1, 2)) or
                     (request_type == 0x82 and index in (0, 0x80, 1, 0x81, 0x82, 3, 0x83)))
            if valid:
                return b"\0\0"
        if request_type == 0x81 and request == 10 and value == 0 and index in (0, 1, 2) and size == 1:
            return b"\0"
        if request_type == 1 and request == 11 and value == size == 0 and index in (0, 1, 2):
            return b""
        if request_type == 2 and request == 1 and value == size == 0 and index in (1, 0x81, 0x82, 3, 0x83):
            return b""  # Endpoints do not persist a halt or USB data toggle.
        if request_type == 0xA1 and request == 0x21 and value == 0 and index == 1 and size == 7:
            return struct.pack("<IBBB", *self.coding)
        if request_type == 0x21 and request == 0x20 and value == 0 and index == 1 and size == 7:
            coding = struct.unpack("<IBBB", urb.data)
            baud, stop, parity, bits = coding
            if not (1 <= baud <= 5000000 and stop in (0, 1, 2) and
                    parity in (0, 1, 2) and 5 <= bits <= 8):
                raise Stall()
            if self.line_state & 1:
                await self.backend.configure(coding)
            self.coding = coding
            return b""
        if request_type == 0x21 and request == 0x22 and index == 1 and size == 0 and value <= 3:
            if value & 1 and not self.line_state & 1:
                await self.backend.configure(self.coding)
            self.line_state = value
            # No modem-signal inputs exist in the network UART contract.
            self.notification = bytes.fromhex("a1200000010002000000")
            self.flush_notification()
            return b""
        raise Stall()

    async def output_worker(self, ep):
        while True:
            urb = await self.outputs[ep].get()
            if self.pending.get(urb.seq) is not urb:
                continue
            urb.active = True
            try:
                if ep == 0:
                    self.complete(urb, await self.control(urb))
                elif ep == 1:
                    if not 1 <= urb.size <= DAP_PACKET_SIZE:
                        raise Stall()
                    while len(self.responses) >= DAP_PACKET_COUNT:
                        self.response_space.clear()
                        await self.response_space.wait()
                    # Unlink while waiting for space must not execute SWD later.
                    if self.pending.get(urb.seq) is not urb:
                        continue
                    response = await self.backend.dap(urb.data)
                    self.responses.append(response)
                    self.complete(urb)
                    self.flush_dap()
                else:
                    if not self.line_state & 1:
                        raise Stall()
                    if urb.data:
                        await self.backend.write(urb.data)
                    self.complete(urb)
            except Stall:
                self.complete(urb, status=STALL)

    async def uart_reader(self):
        buffered = b""
        while True:
            delay = 0.01
            urb = self.first_input(3)
            if urb is not None:
                if not buffered:
                    buffered = await self.backend.read(min(256, urb.size))
                # An UNLINK can arrive during network polling. Preserve at most
                # 256 consumed bytes until another UART IN request arrives.
                urb = self.first_input(3)
                if buffered and urb is not None:
                    self.complete(urb, buffered[:urb.size])
                    buffered = buffered[urb.size:]
                    delay = 0
            await asyncio.sleep(delay)

    async def keepalive(self):
        while True:
            await asyncio.sleep(KEEPALIVE_INTERVAL)
            await self.backend.keepalive()

    async def run(self):
        tasks = [asyncio.create_task(self.receive()),
                 asyncio.create_task(self.uart_reader()),
                 asyncio.create_task(self.keepalive())]
        tasks += [asyncio.create_task(self.output_worker(ep)) for ep in self.outputs]
        try:
            done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
            for task in done:
                task.result()
        finally:
            # Interrupt socket I/O before cancelling wrappers around to_thread.
            await self.backend.close()
            for task in tasks:
                task.cancel()
            await asyncio.gather(*tasks, return_exceptions=True)


class Server:
    def __init__(self, device_id, backend_factory):
        self.descriptors = Descriptors(device_id)
        self.backend_factory = backend_factory
        self.imported = False
        self.connections = {}

    async def handle(self, reader, writer):
        task = asyncio.current_task()
        if len(self.connections) >= 8:
            writer.close()
            await writer.wait_closed()
            return
        self.connections[task] = writer
        backend = None
        claimed = False
        try:
            version, operation, status = struct.unpack(">HHI",
                await asyncio.wait_for(reader.readexactly(8), 5))
            if version != VERSION or status:
                raise ProtocolError("invalid USB/IP negotiation")
            if operation == 0x8005:
                count = 0 if self.imported else 1
                writer.write(struct.pack(">HHII", VERSION, 5, 0, count))
                if count:
                    writer.write(self.descriptors.record() +
                                 bytes.fromhex("ff000000 02020100 0a000000"))
                await writer.drain()
            elif operation == 0x8003:
                busid = await asyncio.wait_for(reader.readexactly(32), 5)
                if busid != BUSID or self.imported:
                    writer.write(struct.pack(">HHI", VERSION, 3, 1))
                    await writer.drain()
                    return
                self.imported = claimed = True
                backend = self.backend_factory()
                try:
                    await backend.connect()
                except Exception:
                    writer.write(struct.pack(">HHI", VERSION, 3, 1))
                    await writer.drain()
                    raise
                writer.write(struct.pack(">HHI", VERSION, 3, 0) + self.descriptors.record())
                await writer.drain()
                LOG.info("Attached CMSIS-DAP and target UART on bus 1-1")
                await Session(reader, writer, self.descriptors, backend).run()
            else:
                raise ProtocolError("unsupported USB/IP negotiation operation")
        except (ProtocolError, uart.ProbeError, OSError, asyncio.IncompleteReadError) as error:
            LOG.warning("USB/IP connection ended: %s", error)
        except Exception:
            LOG.exception("Unexpected USB/IP session failure")
        finally:
            if backend is not None:
                await backend.close()
            if claimed:
                self.imported = False
                LOG.info("Detached bus 1-1; authenticated DAP/UART sessions closed")
            writer.close()
            try:
                await asyncio.wait_for(writer.wait_closed(), 2)
            except OSError:
                pass
            self.connections.pop(task, None)

    async def close(self):
        tasks = list(self.connections)
        for task, writer in list(self.connections.items()):
            writer.close()
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
