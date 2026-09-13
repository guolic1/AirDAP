"""Service boundaries: private persistence, HTTP access and serialized operations."""
import asyncio
import http.client
import json
from pathlib import Path
import sys
import tempfile
import threading
import unittest
from unittest.mock import AsyncMock, MagicMock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from airdap_service import Mount, Service, ServiceError, Store, WebServer
from airdap_network_credential import NetworkCredential

DEVICE = 'ADP-001122334455'


class ServiceTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.store = Store(Path(self.temp.name))
        self.service = Service(self.store, usbip_port=0)

    async def asyncTearDown(self):
        await self.service.close()
        self.temp.cleanup()

    async def test_profile_survives_restart_without_secrets(self):
        credential = NetworkCredential.create(DEVICE, bytes(range(32)))
        await self.service.command('profile', {'device_id': DEVICE, 'host': '192.168.1.2',
                                               'credential': json.loads(credential.to_json())})
        loaded = Store(Path(self.temp.name))
        self.assertEqual(loaded.profile['device_id'], DEVICE)
        self.assertEqual(loaded.credential().psk, credential.psk)
        self.assertNotIn('psk', json.dumps(await self.service.state()))
        self.assertNotIn(credential.psk.hex(), (loaded.path / 'config.json').read_text())

    async def test_invalid_profile_cannot_replace_saved_configuration(self):
        before = self.store.profile.copy()
        for value in ('../../escape', DEVICE + '-NET'):
            with self.assertRaises(ServiceError):
                await self.service.command('profile', {'device_id': value, 'host': 'localhost'})
        self.assertEqual(self.store.profile, before)

    async def test_provisioning_keeps_one_session_until_explicit_cancel(self):
        await self.service.command('profile', {'device_id': DEVICE, 'host': 'localhost'})
        session = MagicMock()
        session.scan = AsyncMock(return_value=[{'ssid':'Example', 'rssi':-42}])
        session.pair = AsyncMock(return_value={'paired':True})
        session.wifi = AsyncMock(return_value={'wifi':'online'})
        session.close = AsyncMock()
        self.service.devices.open_provisioning = AsyncMock(return_value=session)
        data = {'device_id':DEVICE, 'transport':'usb'}
        await self.service.command('provision-start', data)
        await self.service.job_task
        for action, extra in [('provision-pair', {'confirm':True}),
                              ('provision-scan', {}),
                              ('provision-wifi', {'ssid':'Example', 'password':'example-only', 'confirm':True})]:
            await self.service.command(action, data | extra)
            await self.service.job_task
            self.assertEqual(self.service.job['state'], 'succeeded')
            self.assertTrue((await self.service.state())['provisioning']['active'])
            session.close.assert_not_awaited()
        self.assertTrue(self.store.credential_path().exists())
        self.assertNotIn('example-only', json.dumps(await self.service.state()))
        with self.assertRaises(ServiceError):
            await self.service.command('profile', {'device_id':DEVICE, 'host':'other.local'})
        await self.service.command('provision-cancel', data)
        await self.service.job_task
        session.close.assert_awaited_once()
        self.assertFalse((await self.service.state())['provisioning']['active'])

    async def test_provision_failure_preserves_window_and_allows_retry(self):
        await self.service.command('profile', {'device_id':DEVICE, 'host':'localhost'})
        session = MagicMock(scan=AsyncMock(side_effect=[ServiceError('扫描失败'), []]), close=AsyncMock())
        self.service.devices.open_provisioning = AsyncMock(return_value=session)
        data = {'device_id':DEVICE, 'transport':'ble'}
        await self.service.command('provision-start', data)
        await self.service.job_task
        for expected in ('failed', 'succeeded'):
            await self.service.command('provision-scan', data)
            await self.service.job_task
            self.assertEqual(self.service.job['state'], expected)
            self.assertTrue((await self.service.state())['provisioning']['active'])
        self.assertEqual(self.service.devices.open_provisioning.await_count, 1)

    async def test_job_excludes_other_mutations_and_hides_exception_contents(self):
        release = asyncio.Event()
        async def fail():
            await release.wait()
            raise RuntimeError('secret-password-do-not-expose')
        self.service.start_job('test', fail)
        with self.assertRaises(ServiceError):
            await self.service.command('profile', {'device_id': DEVICE, 'host': 'localhost'})
        release.set()
        await self.service.job_task
        state = await self.service.state()
        self.assertEqual(state['job']['state'], 'failed')
        self.assertNotIn('secret-password', json.dumps(state))

    async def test_ota_requires_matching_image_and_explicit_confirmation(self):
        self.service.devices.ota = AsyncMock()
        with self.assertRaises(ServiceError):
            await self.service.command('ota', {'transport': 'network', 'device_id': DEVICE})
        self.service.devices.ota.assert_not_awaited()

    async def test_information_only_service_never_detaches_another_listener(self):
        self.service.mount.executable = 'test-client'
        self.service.mount.detach = MagicMock()
        await self.service.close()
        self.service.mount.detach.assert_not_called()

    async def test_mount_cleanup_matches_exact_busid_and_tcp_port(self):
        mount = Mount(3242)
        mount.run = MagicMock(return_value='Port 01: used\n -> usbip://127.0.0.1:3242/1-10\n'
                              'Port 02: used\n -> usbip://127.0.0.1:3242/1-1\n'
                              'Port 03: used\n -> usbip://127.0.0.1:3243/1-1\n')
        self.assertEqual(mount.matching_ports(), [2])

    async def test_failed_mount_retries_within_two_seconds(self):
        credential = NetworkCredential.create(DEVICE, bytes(range(32)))
        await self.service.command('profile', {'device_id':DEVICE, 'host':'localhost',
            'auto_attach':False, 'credential':json.loads(credential.to_json())})
        self.service.mount.executable = None
        self.service.mount.detach = MagicMock()
        self.service.mount.attach = MagicMock(side_effect=ServiceError('offline'))
        await self.service.start_bridge()
        with self.assertRaises(ServiceError):
            await self.service.attach()
        attached = asyncio.Event()
        loop = asyncio.get_running_loop()
        self.service.mount.attach.side_effect = lambda: loop.call_soon_threadsafe(attached.set)
        self.store.profile['auto_attach'] = True
        maintenance = asyncio.create_task(self.service.maintain())
        try:
            await asyncio.wait_for(attached.wait(), 2)
            await self.service.job_task
            self.assertEqual(self.service.job['state'], 'succeeded')
        finally:
            maintenance.cancel()
            await asyncio.gather(maintenance, return_exceptions=True)

    async def test_stop_closes_connected_client_before_waiting_for_listener(self):
        credential = NetworkCredential.create(DEVICE, bytes(range(32)))
        await self.service.command('profile', {'device_id':DEVICE, 'host':'localhost',
            'auto_attach':False, 'credential':json.loads(credential.to_json())})
        self.service.mount.executable = None
        await self.service.start_bridge()
        reader, writer = await asyncio.open_connection('127.0.0.1', self.service.usbip_port)
        try:
            await asyncio.sleep(.02)
            self.assertTrue(self.service.bridge.connections)
            await asyncio.wait_for(self.service.stop_bridge(), timeout=1)
            self.assertEqual(await asyncio.wait_for(reader.read(), timeout=1), b'')
        finally:
            writer.close()
            await writer.wait_closed()

    async def test_http_rejects_cross_origin_bad_host_and_missing_token(self):
        server = WebServer(('127.0.0.1', 0), self.service, asyncio.get_running_loop())
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        port = server.server_port
        def request(method, path, body=None, headers=None):
            conn = http.client.HTTPConnection('127.0.0.1', port, timeout=3)
            try:
                conn.request(method, path, body=body, headers=headers or {})
                response = conn.getresponse()
                return response.status, response.read()
            finally:
                conn.close()
        try:
            status, page = await asyncio.to_thread(request, 'GET', '/')
            self.assertEqual(status, 200)
            self.assertIn(server.token.encode(), page)
            self.assertEqual((await asyncio.to_thread(request, 'GET', '/api/state'))[0], 403)
            headers = {'X-AirDAP-Token': server.token}
            self.assertEqual((await asyncio.to_thread(request, 'GET', '/api/state', None, headers))[0], 200)
            for extra in ({'Origin': 'https://evil.example'}, {'Host': 'evil.example'},
                          {'Sec-Fetch-Site': 'cross-site'}):
                self.assertEqual((await asyncio.to_thread(request, 'GET', '/api/state', None,
                                                          headers | extra))[0], 403)
            status, _ = await asyncio.to_thread(request, 'POST', '/api/profile', '{}', headers)
            self.assertEqual(status, 415)
        finally:
            await asyncio.to_thread(server.shutdown)
            server.server_close()
            thread.join()


if __name__ == '__main__':
    unittest.main()
