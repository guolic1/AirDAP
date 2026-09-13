"""A SIGTERM must close live USB/IP clients before awaiting listener shutdown."""
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time

TOOLS = Path(__file__).resolve().parents[3] / 'tools'
sys.path.insert(0, str(TOOLS))
from airdap_network_credential import load_or_create_credential


def main():
    assert os.name == 'posix'
    with tempfile.TemporaryDirectory() as folder:
        key = Path(folder) / 'credential.json'
        load_or_create_credential(key, 'ADP-001122334455')
        with socket.socket() as sock:
            sock.bind(('127.0.0.1', 0))
            port = sock.getsockname()[1]
        process = subprocess.Popen([sys.executable, str(TOOLS / 'airdap-usbip.py'),
            '127.0.0.1', '--credential', str(key), '--port', str(port)])
        try:
            deadline = time.monotonic() + 5
            while True:
                try:
                    client = socket.create_connection(('127.0.0.1', port), .2)
                    break
                except OSError:
                    if time.monotonic() > deadline:
                        raise
                    time.sleep(.05)
            with client:
                time.sleep(.05)
                process.terminate()
                assert process.wait(2) == 0
                assert client.recv(1) == b''
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
    print('PASS: SIGTERM closes a connected USB/IP peer within two seconds')


if __name__ == '__main__':
    main()
