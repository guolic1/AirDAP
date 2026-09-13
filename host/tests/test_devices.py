from contextlib import contextmanager
import asyncio
import json
import os
from pathlib import Path
import struct
import sys
import unittest
from unittest.mock import AsyncMock, MagicMock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from airdap_service_devices import Devices, ServiceError, usb_wifi, wifi_fields, usb_update, net_update, BleProvisioning, UsbProvisioning
from airdap_service import Service, Store
from airdap_network_credential import NetworkCredential
import tempfile

DEVICE = 'ADP-001122334455'


class UsbWifiTests(unittest.TestCase):
    def test_prompt_input_and_dhcp_confirmation(self):
        link = MagicMock()
        with patch('airdap_service_devices.shell.read_until', return_value=b''), \
             patch('airdap_service_devices.shell.read_until_prompt', return_value=b'wifi: credentials saved; reconnect requested\n'), \
             patch('airdap_service_devices.shell.run_command', side_effect=[b'wifi=connecting\n', b'wifi=online\n']), \
             patch('airdap_service_devices.time.sleep'):
            self.assertEqual(usb_wifi(link, 'ExampleSSID', 'example-only'), {'wifi':'online'})
        self.assertEqual([call.args[0] for call in link.write.call_args_list],
                         [b'wifi set\n', b'ExampleSSID\n', b'example-only\n'])

    def test_saved_is_not_connected(self):
        with patch('airdap_service_devices.shell.read_until', return_value=b''), \
             patch('airdap_service_devices.shell.read_until_prompt', return_value=b'wifi: credentials saved; reconnect requested\n'):
            with self.assertRaisesRegex(ServiceError, '配置已保存'):
                usb_wifi(MagicMock(), 'ExampleSSID', '', timeout=0)

    def test_controls_and_usb_non_ascii_are_rejected_before_writing(self):
        for ssid, password in [('ok', 'bad\nrestart'), ('bad\x00', ''), ('中文', '')]:
            link = MagicMock()
            with self.assertRaises(ServiceError):
                usb_wifi(link, ssid, password)
            link.write.assert_not_called()
        wifi_fields('中文', '', 'ble')


class BleTests(unittest.IsolatedAsyncioTestCase):
    async def test_control_waits_for_device_event_loop_and_rejects_failed_cancel(self):
        session = BleProvisioning(DEVICE, MagicMock())
        session.security = MagicMock()
        session.security.encrypt_data.side_effect = lambda value: value
        session.security.decrypt_data.side_effect = lambda value: value
        session.link = MagicMock(send_data=AsyncMock(side_effect=[
            bytes([2,1,1,0]), bytes([2,0,1,1]), bytes([2,0,4,0])]))
        await session.control(1)
        self.assertEqual([call.args[1].encode('latin-1') for call in session.link.send_data.call_args_list],
                         [bytes([2,1]), bytes([2,0])])
        with self.assertRaises(ServiceError):
            await session.control(4)

    async def test_usb_pair_checks_fingerprint_and_never_uses_command_arguments(self):
        from airdap_network_credential import NetworkCredential
        credential = NetworkCredential.create(DEVICE, bytes(range(32)))
        session = UsbProvisioning(DEVICE)
        session.link = MagicMock()
        for fingerprint, accepted in [(credential.fingerprint, True), (bytes(32), False)]:
            with patch('airdap_service_devices.shell.read_until'), \
                 patch('airdap_service_devices.shell.read_until_prompt',
                       return_value=b'fingerprint=' + fingerprint.hex().encode() + b'\n'):
                if accepted:
                    self.assertTrue((await session.pair(credential))['paired'])
                else:
                    with self.assertRaises(ServiceError):
                        await session.pair(credential)
        writes = [call.args[0] for call in session.link.write.call_args_list]
        self.assertEqual(writes[:2], [b'wifi pair\n', credential.psk.hex().encode() + b'\n'])
        self.assertEqual(writes[-1], b'\x03')

    async def test_ble_open_preserves_safe_session_error(self):
        esp = MagicMock(get_transport=AsyncMock(return_value=MagicMock()),
            get_sec_patch_ver=AsyncMock(return_value=1),
            establish_session=AsyncMock(return_value=True))
        session = BleProvisioning(DEVICE, esp)
        session.control = AsyncMock(side_effect=ServiceError('设备配网会话响应超时。'))
        with self.assertRaisesRegex(ServiceError, '^设备配网会话响应超时。$'):
            await session.open()
        self.assertFalse(session.managed)
        self.assertIsNone(session.heartbeat)

    async def test_persistent_ble_scan_pair_wifi_retry_and_cancel(self):
        from airdap_network_credential import NetworkCredential
        credential = NetworkCredential.create(DEVICE, bytes(range(32)))
        security = MagicMock()
        security.encrypt_data.side_effect = lambda value: value
        security.decrypt_data.side_effect = lambda value: value
        control_commands = []
        async def send(endpoint, data):
            self.assertEqual(endpoint, 'airdap-pair')
            request = data.encode('latin-1')
            if request[0] == 2:
                control_commands.append(request[1])
                return bytes([2, 0, request[1], 1]).decode('latin-1')
            self.assertEqual(request, b'\x01' + credential.psk)
            return credential.fingerprint.decode('latin-1')
        link = MagicMock(send_data=AsyncMock(side_effect=send), disconnect=AsyncMock())
        esp = MagicMock(get_transport=AsyncMock(return_value=link),
            get_sec_patch_ver=AsyncMock(return_value=1), get_security=MagicMock(return_value=security),
            establish_session=AsyncMock(return_value=True),
            scan_wifi_APs=AsyncMock(return_value=[{'ssid':'中文'.encode().decode('latin-1'), 'rssi':-40}]),
            send_wifi_config=AsyncMock(return_value=True), apply_wifi_config=AsyncMock(return_value=True),
            get_wifi_config=AsyncMock(side_effect=['failed','connected']))
        session = BleProvisioning(DEVICE, esp)
        await session.open()
        try:
            self.assertEqual((await session.scan())[0]['ssid'], '中文')
            self.assertTrue((await session.pair(credential))['paired'])
            with self.assertRaises(ServiceError):
                await session.wifi('中文', 'example-only')
            self.assertEqual(await session.wifi('中文', 'example-only'), {'wifi':'online'})
            self.assertEqual(control_commands, [1,2,2,3,3])
            esp.get_transport.assert_awaited_once()
            link.disconnect.assert_not_awaited()
        finally:
            await session.close()
        link.disconnect.assert_awaited_once()
        self.assertEqual(control_commands[-1], 4)

    async def test_usb_scan_parses_ssid_metadata_and_keeps_connection(self):
        session = UsbProvisioning(DEVICE)
        session.link = MagicMock()
        with patch('airdap_service_devices.shell.run_command', side_effect=[
                b'wifi-scan=2\n', b'ap=41,001122334455,6,-70,0\n',
                b'ap=e4b8ade69687,112233445566,11,-40,3\n']):
            found = await session.scan()
            self.assertEqual(found[0], {'ssid':'中文', 'bssid':'112233445566', 'channel':11,
                                       'rssi':-40, 'auth':'WPA2_PSK'})
            self.assertEqual(found[1]['auth'], 'Open')
            self.assertIsNotNone(session.link)

    async def test_explicit_idf_path_is_visible_to_upstream_proto_loader(self):
        from airdap_service_devices import pair
        observed = []
        def load(path):
            observed.append(os.environ.get('IDF_PATH'))
            raise ImportError('stop before hardware dependencies')
        with patch.dict(os.environ, {}, clear=True), patch.object(pair, 'load_esp_prov', side_effect=load):
            with self.assertRaises(ServiceError):
                Devices(idf_path=Path('/configured/idf')).ble_client()
            self.assertEqual(observed, [str(Path('/configured/idf'))])
            self.assertNotIn('IDF_PATH', os.environ)

    async def test_wifi_session_disconnects_on_success_and_apply_failure(self):
        for accepted in (True, False):
            esp = MagicMock()
            link = MagicMock(disconnect=AsyncMock())
            esp.get_transport = AsyncMock(return_value=link)
            esp.get_sec_patch_ver = AsyncMock(return_value=1)
            esp.establish_session = AsyncMock(return_value=True)
            esp.send_wifi_config = AsyncMock(return_value=True)
            esp.apply_wifi_config = AsyncMock(return_value=accepted)
            esp.get_wifi_config = AsyncMock(return_value='connected')
            devices = Devices()
            devices.esp = esp
            if accepted:
                result = await devices.wifi(DEVICE, 'ble', '中文', 'example-only')
                self.assertEqual(result['wifi'], 'online')
            else:
                with self.assertRaisesRegex(ServiceError, '可能已保存'):
                    await devices.wifi(DEVICE, 'ble', '中文', 'example-only')
                esp.get_wifi_config.assert_not_awaited()
            link.disconnect.assert_awaited_once()

    async def test_wifi_is_not_success_before_dhcp(self):
        devices = Devices()
        esp = MagicMock()
        devices.esp = esp
        link = MagicMock(disconnect=AsyncMock())
        for name, value in [('get_transport',link),('get_sec_patch_ver',1),
                            ('establish_session',True),('send_wifi_config',True),
                            ('apply_wifi_config',True),('get_wifi_config','failed')]:
            setattr(esp, name, AsyncMock(return_value=value))
        with self.assertRaises(ServiceError):
            await devices.wifi(DEVICE, 'ble', 'ExampleSSID', '')
        link.disconnect.assert_awaited_once()


def image_bytes():
    image = bytearray(128)
    image[0] = 0xe9
    struct.pack_into('<H', image, 12, 9)
    struct.pack_into('<I', image, 32, 0xabcd5432)
    image[48:53] = b'vtest'
    return bytes(image)


class OtaTests(unittest.IsolatedAsyncioTestCase):
    async def test_bridge_allows_image_inspection_but_blocks_flash(self):
        with tempfile.TemporaryDirectory() as folder:
            devices = Devices()
            devices.ota = AsyncMock()
            service = Service(Store(Path(folder)), devices=devices, usbip_port=0)
            service.mount = MagicMock(executable=None)
            try:
                credential = NetworkCredential.create(DEVICE, bytes(range(32)))
                await service.command('profile', {'device_id':DEVICE, 'host':'localhost',
                    'auto_attach':False, 'credential':json.loads(credential.to_json())})
                await service.start_bridge()
                meta = await service.stage_image(image_bytes())
                self.assertEqual(meta['version'], 'vtest')
                self.assertTrue((await service.state())['bridge']['listening'])
                with self.assertRaisesRegex(ServiceError, '停止 USB 桥接'):
                    await service.command('ota', {'device_id':DEVICE, 'transport':'usb',
                        'confirm':True, 'sha256':meta['sha256']})
                devices.ota.assert_not_awaited()
                release = asyncio.Event()
                service.start_job('等待操作', release.wait)
                try:
                    with self.assertRaisesRegex(ServiceError, '当前操作尚未结束'):
                        await service.stage_image(image_bytes())
                    self.assertEqual(service.image_meta, meta)
                finally:
                    release.set()
            finally:
                await service.close()

    async def test_uploaded_image_never_writes_until_matching_confirmation(self):
        with tempfile.TemporaryDirectory() as folder:
            devices = Devices()
            devices.ota = AsyncMock(return_value={'version':'vtest'})
            service = Service(Store(Path(folder)), devices=devices)
            await service.command('profile', {'device_id': DEVICE, 'host':'localhost'})
            meta = await service.stage_image(image_bytes())
            self.assertEqual(meta['version'], 'vtest')
            devices.ota.assert_not_awaited()
            for wrong in ({'confirm':False, 'sha256':meta['sha256']},
                          {'confirm':True, 'sha256':'stale'}):
                with self.assertRaises(ServiceError):
                    await service.command('ota', {'device_id':DEVICE, 'transport':'usb'} | wrong)
            devices.ota.assert_not_awaited()
            await service.command('ota', {'device_id':DEVICE, 'transport':'usb', 'confirm':True, 'sha256':meta['sha256']})
            await service.job_task
            devices.ota.assert_awaited_once()
            self.assertEqual(service.job['state'], 'succeeded')
            self.assertIsNone(service.image)

    async def test_network_update_uses_commit_reboot_and_startup_verification(self):
        events = []
        client = MagicMock()
        client.__enter__.return_value = client
        before = MagicMock()
        after = net_update.OtaInfo(1000, 2000, 1000, True, 'vtest')
        client.reboot.side_effect = lambda: events.append('reboot')
        with patch.object(net_update, 'Client', return_value=client), \
             patch.object(net_update, 'upload', side_effect=lambda *a: events.append('upload') or before), \
             patch.object(net_update, 'wait_updated', side_effect=lambda *a: events.append('verify') or after) as wait:
            result = await Devices().ota(DEVICE, 'localhost', object(), 'network', image_bytes(), lambda *_: None)
            self.assertEqual(events, ['upload','reboot','verify'])
            self.assertTrue(result['confirmed'])
            self.assertEqual(wait.call_args.args[2:4], (before, 'vtest'))

    async def test_usb_reconnect_to_old_version_is_a_failure(self):
        link = MagicMock()
        link.query.return_value = usb_update.OtaInfo(1, 1, 1000, 'old-version')
        @contextmanager
        def transport(*_args, **_kwargs):
            yield link
        # USB modules are only imported after writing; provide their read-only API shape
        # so this test also runs on Linux hosts without a PyUSB installation.
        import types
        usb = types.ModuleType('usb')
        usb.core = types.ModuleType('usb.core')
        usb.core.USBError = OSError
        usb.util = types.ModuleType('usb.util')
        with patch('airdap_service_devices.usb_transport', transport), \
             patch.object(usb_update, 'upload_image') as upload, \
             patch.object(usb_update, 'wait_for_disconnect'), \
             patch.dict(sys.modules, {'usb':usb, 'usb.core':usb.core, 'usb.util':usb.util}):
            with self.assertRaisesRegex(ServiceError, '版本与镜像不一致'):
                await Devices().ota(DEVICE, 'localhost', None, 'usb', image_bytes(), lambda *_: None)
            upload.assert_called_once()

    async def test_network_verification_failure_does_not_retry_upload(self):
        client = MagicMock()
        client.__enter__.return_value = client
        with patch.object(net_update, 'Client', return_value=client), \
             patch.object(net_update, 'upload') as upload, \
             patch.object(net_update, 'wait_updated', side_effect=net_update.UpdateError('not confirmed')):
            with self.assertRaises(net_update.UpdateError):
                await Devices().ota(DEVICE, 'localhost', object(), 'network', image_bytes(), lambda *_: None)
            upload.assert_called_once()
            client.reboot.assert_called_once()


if __name__ == '__main__':
    unittest.main()
