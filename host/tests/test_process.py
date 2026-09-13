"""POSIX process proof: saved bridge restores after SIGTERM/restart, HTTP optional."""
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import tempfile
import time
import urllib.request

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from airdap_service import Store
from airdap_network_credential import NetworkCredential


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def wait_list(port):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(('127.0.0.1', port), 1) as sock:
                sock.sendall(bytes.fromhex('0111800500000000'))
                reply = bytearray()
                while len(reply) < 12:
                    reply.extend(sock.recv(12-len(reply)))
                assert reply == bytes.fromhex('011100050000000000000001'), reply
                return
        except OSError:
            time.sleep(.1)
    raise AssertionError('bridge did not restore')


def main():
    assert os.name == 'posix', 'run this lifecycle test on Linux'
    with tempfile.TemporaryDirectory() as folder:
        port, web_port = free_port(), free_port()
        store = Store(Path(folder))
        serial = 'ADP-001122334455'
        credential = NetworkCredential.create(serial, bytes(range(32)))
        keyfile = store.credential_path(serial)
        keyfile.parent.mkdir()
        keyfile.write_text(credential.to_json())
        keyfile.chmod(0o600)
        store.save({'device_id':serial, 'host':'127.0.0.1', 'auto_attach':False, 'bridge_enabled':True})
        for http in (True, False):
            args = [sys.executable, str(Path(__file__).resolve().parents[1] / 'airdap-service.py'),
                    '--data-dir', folder, '--usbip-port', str(port), '--http-port', str(web_port)]
            if not http:
                args += ['--no-http']
            proc = subprocess.Popen(args)
            try:
                wait_list(port)
                if http:
                    page = urllib.request.urlopen(f'http://127.0.0.1:{web_port}/').read().decode()
                    token = re.search(r'name="api-token" content="([^"]+)"', page)[1]
                    request = urllib.request.Request(f'http://127.0.0.1:{web_port}/api/state', headers={'X-AirDAP-Token':token})
                    state = json.load(urllib.request.urlopen(request))
                    assert state['bridge']['listening'] and state['profile']['bridge_enabled']
                else:
                    with socket.socket() as sock:
                        assert sock.connect_ex(('127.0.0.1', web_port)) != 0
            finally:
                proc.terminate()
                assert proc.wait(10) == 0
            for closed_port in (port, web_port):
                with socket.socket() as sock:
                    assert sock.connect_ex(('127.0.0.1', closed_port)) != 0
        assert Store(Path(folder)).profile['bridge_enabled']
    print('PASS: SIGTERM drains service; ports close; persisted bridge restores; --no-http stays closed')


if __name__ == '__main__':
    main()
