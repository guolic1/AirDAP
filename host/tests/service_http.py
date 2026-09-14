"""Native executable acceptance tests. Uses disposable state; never accesses hardware."""
import http.client
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'firmware/tools'))
from airdap_network_credential import NetworkCredential

BINARY = os.environ.get('AIRDAP_NATIVE_SERVICE', str(Path(__file__).resolve().parents[1] / 'target/debug' / ('airdap-service.exe' if os.name == 'nt' else 'airdap-service')))
DEVICE = 'ADP-001122334455'

def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]

class NativeService(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.port, self.usb_port = free_port(), free_port()
        self.args = [BINARY, '--data-dir', self.temp.name, '--http-port', str(self.port), '--usbip-port', str(self.usb_port)]
        self.process = None
        self.addCleanup(self.stop)
        self.start()

    def start(self):
        self.process = subprocess.Popen(self.args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        end = time.monotonic()+10
        while True:
            try:
                status, page = self.request('GET', '/', token=False)
                self.assertEqual(status, 200)
                self.token = re.search('name="api-token" content="([^"]+)"', page.decode()).group(1)
                break
            except OSError:
                if self.process.poll() is not None or time.monotonic() >= end:
                    raise
                time.sleep(.05)

    def stop(self):
        if self.process and self.process.poll() is None:
            self.process.terminate()
            self.process.wait(timeout=15)

    def request(self, method, path, body=None, token=True, headers=None):
        h = dict(headers or {})
        if token:
            h.setdefault('X-AirDAP-Token', self.token)
        if isinstance(body, dict):
            body = json.dumps(body).encode()
            h.setdefault('Content-Type', 'application/json')
        elif body is not None:
            h.setdefault('Content-Type', 'application/octet-stream')
        connection = http.client.HTTPConnection('127.0.0.1', self.port, timeout=12)
        try:
            connection.request(method, path, body, h)
            response = connection.getresponse()
            self.assertEqual(response.getheader('X-Frame-Options'), 'DENY')
            return response.status, response.read()
        finally:
            connection.close()

    def state(self):
        status, data = self.request('GET','/api/state')
        self.assertEqual(status,200)
        return json.loads(data)

    def job(self):
        end = time.monotonic()+5
        while (state := self.state())['job']['state'] == 'running':
            self.assertLess(time.monotonic(), end)
            time.sleep(.02)
        self.assertEqual(state['job']['state'], 'succeeded', state['job'])
        return state

    def profile(self):
        credential = json.loads(NetworkCredential.create(DEVICE, bytes([0x11])*32).to_json())
        status,_ = self.request('POST','/api/profile', {'device_id':DEVICE,'host':'127.0.0.1','auto_attach':False,'credential':credential})
        self.assertEqual(status,200)

    def test_web_origin_token_and_request_limits(self):
        self.assertEqual(self.request('GET','/api/state',token=False)[0],403)
        self.assertEqual(self.request('GET','/',token=False,headers={'Host':'evil.example'})[0],403)
        self.assertEqual(self.request('POST','/api/start',{},headers={'Origin':'http://evil.example'})[0],403)
        self.assertEqual(self.request('GET','/api/state',headers={'Sec-Fetch-Site':'same-site'})[0],403)
        self.assertEqual(self.request('POST','/api/profile',b'x'*16385,headers={'Content-Type':'application/json'})[0],413)
        self.assertEqual(self.request('POST','/api/profile',b'{',headers={'Content-Type':'application/json'})[0],400)
        self.assertEqual(self.state()['profile'],{})

    def test_airdap_localhost_with_selected_port(self):
        host = f'airdap.localhost:{self.port}'
        headers = {'Host': host, 'Origin': f'http://{host}', 'Sec-Fetch-Site': 'same-origin'}
        status, page = self.request('GET', '/', token=False, headers={'Host': host})
        self.assertEqual(status, 200)
        self.assertIn(self.token.encode(), page)
        self.assertEqual(self.request('GET', '/api/state', headers=headers)[0], 200)
        self.assertEqual(self.request('POST', '/api/profile', {
            'device_id': DEVICE, 'host': '127.0.0.1', 'auto_attach': False,
        }, headers=headers)[0], 200)
        self.assertEqual(self.state()['profile']['device_id'], DEVICE)
        self.assertEqual(self.request('GET', '/api/state', token=False, headers=headers)[0], 403)
        for rejected in (f'evil.airdap.localhost:{self.port}',
                         f'airdap.localhost.evil.example:{self.port}',
                         f'airdap.localhost:{self.port % 65535 + 1}', 'airdap.localhost'):
            with self.subTest(host=rejected):
                self.assertEqual(self.request('GET', '/', token=False, headers={'Host': rejected})[0], 403)
        for origin in (f'http://localhost:{self.port}', f'http://airdap.localhost:{self.port % 65535 + 1}',
                       f'https://{host}', 'http://evil.example'):
            with self.subTest(origin=origin):
                self.assertEqual(self.request('GET', '/api/state', headers=headers | {'Origin': origin})[0], 403)

    def test_profile_credentials_and_singleton_survive_restart(self):
        self.profile()
        self.assertTrue(self.state()['credential_present'])
        other = subprocess.run(self.args,stdout=subprocess.PIPE,stderr=subprocess.PIPE,timeout=10)
        self.assertNotEqual(other.returncode,0)
        self.assertNotIn(b'11111111111111111111111111111111',other.stderr)
        old_token = self.token
        self.stop(); self.start()
        self.assertNotEqual(self.token,old_token)
        self.assertEqual(self.state()['profile']['device_id'],DEVICE)
        self.assertNotIn('psk',json.dumps(self.state()))
        self.assertEqual(self.request('GET','/api/state',headers={'X-AirDAP-Token':old_token})[0],403)

    def test_bridge_restore_and_stage_image_while_listening(self):
        self.profile()
        self.assertEqual(self.request('POST','/api/start',{})[0],202)
        self.assertTrue(self.job()['bridge']['listening'])
        image = bytearray(80); image[0]=0xe9; image[12]=9
        image[32:36]=(0xabcd5432).to_bytes(4,'little'); image[48:53]=b'vtest'
        self.assertEqual(self.request('POST','/api/image',bytes(image))[0],200)
        self.assertEqual(self.state()['image']['version'],'vtest')
        self.assertEqual(self.request('POST','/api/ota',{'device_id':DEVICE,'transport':'network','confirm':True,'sha256':self.state()['image']['sha256']})[0],409)
        self.stop(); self.start()
        end = time.monotonic()+5
        while not self.state()['bridge']['listening']:
            self.assertLess(time.monotonic(),end); time.sleep(.02)
        self.assertEqual(self.request('POST','/api/stop',{})[0],202)
        self.assertFalse(self.job()['bridge']['listening'])
        with self.assertRaises(OSError):
            socket.create_connection(('127.0.0.1',self.usb_port),timeout=.5)
        self.assertFalse(json.loads((Path(self.temp.name)/'config.json').read_text())['bridge_enabled'])

    def test_invalid_target_cannot_change_saved_profile_or_begin_write(self):
        self.profile()
        self.assertEqual(self.request('POST','/api/profile',{'device_id':DEVICE,'host':'http://localhost/x'})[0],409)
        self.assertEqual(self.state()['profile']['host'],'127.0.0.1')
        self.assertEqual(self.request('POST','/api/provision-start',{'device_id':'ADP-FFFFFFFFFFFF','transport':'ble'})[0],409)
        self.assertEqual(self.request('POST','/api/ota',{'device_id':DEVICE,'transport':'network'})[0],409)
        self.assertEqual(self.state()['job']['state'],'idle')

if __name__ == '__main__':
    unittest.main()
