"""Real native-process / TLS-PSK / USB-IP boundary tests; no physical device."""
import json
import os
from pathlib import Path
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'firmware/tools'))
from airdap_network_credential import NetworkCredential

BINARY = os.environ.get('AIRDAP_NATIVE_BRIDGE', str(Path(__file__).resolve().parents[1] / 'target/debug/examples' / ('bridge.exe' if os.name == 'nt' else 'bridge')))
DEVICE = 'ADP-001122334455'
FRAME = struct.Struct('>4sBBHIIHH')

def exact(connection, size):
    result = b''
    while len(result) < size:
        chunk = connection.recv(size-len(result))
        if not chunk:
            raise EOFError('connection closed')
        result += chunk
    return result

class Firmware:
    def __init__(self, port=0):
        self.listener = socket.socket()
        if os.name != 'nt':
            self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(('127.0.0.1', port))
        self.listener.listen()
        self.port = self.listener.getsockname()[1]
        self.closed = False
        self.sockets = []
        self.uart = bytearray()
        self.dap_started = threading.Event()
        self.delay_dap = False
        self.thread = threading.Thread(target=self.accept, daemon=True)
        self.thread.start()

    def accept(self):
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = context.maximum_version = ssl.TLSVersion.TLSv1_3
        context.set_psk_server_callback(lambda identity: bytes([0x11])*32 if identity == 'AIRDAP:'+DEVICE else b'')
        while not self.closed:
            try:
                raw, _ = self.listener.accept()
                link = context.wrap_socket(raw, server_side=True)
                self.sockets.append(link)
                threading.Thread(target=self.run, args=(link,), daemon=True).start()
            except OSError:
                if not self.closed:
                    raise

    def run(self, link):
        try:
            while not self.closed:
                magic, version, kind, flags, session, sequence, size, reserved = FRAME.unpack(exact(link, 20))
                assert (magic, version, flags, reserved) == (b'ADAP', 1, 0, 0)
                data = exact(link, size)
                response_kind = kind
                if kind == 1:
                    response = bytes(16) + struct.pack('>I', 31) + DEVICE.encode() + b'vtest'
                elif kind == 2:
                    assert data in (b'', b't'*32)
                    response = struct.pack('>I', 7) + b't'*32
                elif kind == 3:
                    self.dap_started.set()
                    if self.delay_dap:
                        time.sleep(1)
                    response_kind = 4
                    response = data[:1] + b'\x02OK'
                elif kind == 5:
                    response_kind = 6
                    opcode, payload = data[0], data[1:]
                    response = bytes([opcode])
                    if opcode == 0x13:
                        self.uart.extend(payload)
                        response += len(payload).to_bytes(2, 'big')
                    elif opcode == 0x14:
                        size = int.from_bytes(payload, 'big')
                        response += bytes(4) + self.uart[:size]
                        del self.uart[:size]
                    else:
                        assert opcode in (0x11, 0x12)
                else:
                    assert kind == 7
                    response = b''
                link.sendall(FRAME.pack(b'ADAP', 1, response_kind, 0, session, sequence, len(response), 0)+response)
        except (OSError, EOFError):
            pass
        finally:
            link.close()

    def close(self):
        self.closed = True
        try:
            self.listener.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.listener.close()
        for link in self.sockets:
            try:
                link.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            link.close()
        self.thread.join(timeout=2)

class NativeBridge(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        path = Path(self.temp.name)/'credential.json'
        path.write_text(NetworkCredential.create(DEVICE, bytes([0x11])*32).to_json())
        path.chmod(0o600)
        self.firmware = Firmware()
        self.addCleanup(self.firmware.close)
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            self.port = sock.getsockname()[1]
        self.process = subprocess.Popen([BINARY, '127.0.0.1', '--credential', str(path), '--port', str(self.port),
            '--dap-port', str(self.firmware.port), '--uart-port', str(self.firmware.port)],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.addCleanup(self.stop)
        end = time.monotonic()+5
        while True:
            try:
                self.peer = socket.create_connection(('127.0.0.1', self.port), timeout=3)
                break
            except OSError:
                if time.monotonic() >= end or self.process.poll() is not None:
                    raise
                time.sleep(.05)
        self.addCleanup(self.peer.close)
        self.peer.sendall(struct.pack('>HHI', 0x111, 0x8003, 0)+b'1-1'.ljust(32, b'\0'))
        self.assertEqual(exact(self.peer, 8), struct.pack('>HHI', 0x111, 3, 0))
        self.assertEqual(len(exact(self.peer, 312)), 312)

    def stop(self):
        if self.process.poll() is None:
            self.process.terminate()
        self.process.wait(timeout=5)

    def submit(self, seq, ep, direction, size=0, data=b'', setup=bytes(8)):
        self.peer.sendall(struct.pack('>IIIIIIiiii8s', 1, seq, 0x10001, direction, ep, 0, size, 0, 0, 0, setup)+data)

    def reply(self):
        header = exact(self.peer, 48)
        kind, seq = struct.unpack_from('>II', header)
        status, size = struct.unpack_from('>ii', header, 20)
        return kind, seq, status, size

    def test_dap_uart_and_control_enumeration(self):
        self.submit(1, 0, 1, 18, setup=struct.pack('<BBHHH', 0x80, 6, 0x100, 0, 18))
        self.assertEqual(self.reply(), (3, 1, 0, 18))
        self.assertEqual(exact(self.peer, 18)[:2], b'\x12\x01')
        self.submit(2, 1, 1, 508)
        self.submit(3, 1, 0, 2, b'\x00\x01')
        self.assertEqual(self.reply(), (3, 3, 0, 2))
        self.assertEqual(self.reply(), (3, 2, 0, 4))
        self.assertEqual(exact(self.peer, 4), b'\x00\x02OK')
        self.submit(4, 0, 0, setup=struct.pack('<BBHHH', 0x21, 0x22, 1, 1, 0))
        self.assertEqual(self.reply(), (3, 4, 0, 0))
        self.submit(5, 3, 0, 5, b'hello')
        self.assertEqual(self.reply(), (3, 5, 0, 5))
        self.submit(6, 3, 1, 64)
        self.assertEqual(self.reply(), (3, 6, 0, 5))
        self.assertEqual(exact(self.peer, 5), b'hello')

    def test_unlink_idle_input_does_not_close_connection(self):
        self.submit(10, 1, 1, 64)
        self.peer.sendall(struct.pack('>IIIIII24x', 2, 11, 0x10001, 0, 0, 10))
        self.assertEqual(self.reply()[:3], (4, 11, -104))
        self.submit(12, 0, 1, 1, setup=struct.pack('<BBHHH', 0x80, 8, 0, 0, 1))
        self.assertEqual(self.reply(), (3, 12, 0, 1))
        self.assertEqual(exact(self.peer, 1), b'\0')

    def test_inflight_unlink_closes_uncertain_session(self):
        self.firmware.delay_dap = True
        self.submit(20, 1, 0, 2, b'\x00\x01')
        self.assertTrue(self.firmware.dap_started.wait(2))
        self.peer.sendall(struct.pack('>IIIIII24x', 2, 21, 0x10001, 0, 0, 20))
        # The UNLINK response may be discarded as the poisoned session closes.
        received = b''
        while chunk := self.peer.recv(512):
            received += chunk
        self.assertLessEqual(len(received), 48)

    def test_upstream_disconnect_removes_usbip_session(self):
        self.firmware.close()
        started = time.monotonic()
        self.assertEqual(self.peer.recv(48), b'')
        self.assertLess(time.monotonic()-started, 2.5)

if __name__ == '__main__':
    unittest.main()
